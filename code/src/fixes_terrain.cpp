// Terrain diagnostics (TerrainLog): statistics, hole checks and debug keys
// for finding why pieces of distant terrain go missing at long view
// distances.
//
// The game's terrain (object at the terrain manager, per-frame update
// 0x50cdc0) is a tree of nodes. Each node is a 16x16-cell block with up to
// 256 children (node+4, a 16x16 array of finer nodes, loaded from the track
// file on demand by 0x483440). Per frame:
//
//  - Setup (0x482d20 -> 0x482d50): frustum test of each node's box
//    (+0x44 min, +0x50 max) through 0x534be0; node+0x40 holds the visible
//    parts: bit 0 the node, bits 1-4 its quadrants, bits 5-20 its 4x4
//    sub-blocks (Z order). 0x483440 picks the level of detail: when the node
//    is fine enough for its distance it is "collapsed" (data+0x121 bit 0)
//    and drawn whole from its own mesh, otherwise its children are used.
//  - Eval (0x4836e0) and Mesh (0x47fec0): vertex activation and mesh builds
//    for changed blocks (data+0x121 bit 1).
//  - Render (0x4820e0): per visible sub-block a second frustum test
//    (0x482990, box from the sub-block table at data+0x17c) sets the bit in
//    data+0x178 the draw pass (0x4818b0 / 0x481a40 / 0x481c00) reads.
//
// A non-collapsed node draws the sub-blocks that still have triangles in
// its own mesh; the others are covered by its children. A collapsed node
// draws its whole mesh. So a visible spot is a hole when nothing there has
// triangles: a collapsed node with an empty mesh, or a sub-block with no
// triangles and no children to cover it. TerrainLog=1 checks that every
// frame and logs what it finds, with statistics every 5 seconds.
//
// Debug keys (TerrainLog=1, game window in front):
//   Ctrl+F9   sub-block culling off/on (0x482990 always "visible")
//   Ctrl+F10  node culling off/on (0x482d50 sees every node fully inside)
//   Ctrl+F11  write the terrain tree (visible nodes) and the camera to the log
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "config.h"
#include "patch.h"

namespace {

constexpr uintptr_t kRenderCall = 0x50D090;  // call 4820e0 (terrain render step), ecx = root node
constexpr uintptr_t kRender = 0x4820E0;
constexpr uintptr_t kCullNodeCall = 0x482DAD;  // call 534be0 in 0x482d50 (node box)
constexpr uintptr_t kCullSubCall = 0x482A62;   // call 534be0 in 0x482990 (sub-block box)
constexpr uintptr_t kCull = 0x534BE0;
constexpr uintptr_t kMeshCount = 0x690444;     // blocks meshed this frame
constexpr uintptr_t kVbCount = 0x69043C;
constexpr uintptr_t kVbBytes = 0x690440;
constexpr uintptr_t kRenderMs = 0x690450;

// Sub-block (row-major index) of each Z-order sub-block, and the first child
// of each Z-order sub-block in the 16x16 child array.
constexpr int kSubEntry[16] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};
constexpr int kSubChild[16] = {0x00, 0x04, 0x40, 0x44, 0x08, 0x0C, 0x48, 0x4C,
                               0x80, 0x84, 0xC0, 0xC4, 0x88, 0x8C, 0xC8, 0xCC};

using ThisVoid = int(__fastcall*)(void* self, void* edx);
using CullFn = int(__fastcall*)(void* self, void* edx, void* a1, void* a2, void* a3, void* a4, int* a5,
                                int* a6, void* a7);

template <typename T>
T At(const void* p, size_t off) {
  T v;
  std::memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof(T));
  return v;
}

bool g_cull_sub_off = false;
bool g_cull_node_off = false;

struct Stats {
  unsigned frames = 0;
  unsigned long long nodes = 0, visible = 0, collapsed = 0, subs_drawn = 0, subs_culled = 0, children_drawn = 0;
  unsigned long long meshes = 0;
  unsigned max_visible = 0, max_meshes = 0;
  unsigned holes_collapsed = 0, holes_sub = 0, holes_child = 0;
  double render_ms = 0, max_render_ms = 0;
  DWORD start = 0;
} g_st;

unsigned g_hole_logs = 0;  // detailed hole lines written (capped)
constexpr unsigned kMaxHoleLogs = 200;

// Vertex count of row-major sub-block e of a node's mesh.
int SubVerts(const uint8_t* data, int e) {
  const uint8_t* t = At<const uint8_t*>(data, 0x17C);
  if (!t) return -1;
  const int end = At<int16_t>(t, e * 12);
  const int begin = e ? At<int16_t>(t, (e - 1) * 12) : 0;
  return end - begin;
}

void LogNode(const char* what, const uint8_t* n, int sub) {
  const uint8_t* d = At<const uint8_t*>(n, 0x38);
  const float* mn = reinterpret_cast<const float*>(n + 0x44);
  const float* mx = reinterpret_cast<const float*>(n + 0x50);
  patch::Log(
      "terrain hole: %s node %p sub %d level %u at %u,%u vis %06x flags %02x verts %d children %s "
      "tex %s box (%.0f %.0f %.0f)-(%.0f %.0f %.0f)",
      what, n, sub, At<uint16_t>(n, 0x20), At<uint16_t>(n, 0x24), At<uint16_t>(n, 0x26),
      At<uint32_t>(n, 0x40), d ? At<uint8_t>(d, 0x121) : 0xFF, d ? At<int16_t>(d, 0x13C) : -1,
      At<void*>(n, 4) ? "yes" : "no", (At<void*>(n, 0x3C) && At<void*>(At<uint8_t*>(n, 0x3C), 8)) ? "yes" : "no",
      mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
}

// One frame's walk over the visible part of the tree, after Render, the way
// the draw pass will go through it.
void Walk(const uint8_t* n, int depth) {
  if (!n || depth > 12) return;
  ++g_st.nodes;
  const uint32_t vis = At<uint32_t>(n, 0x40);
  if (!vis) return;
  ++g_st.visible;
  const uint8_t* d = At<const uint8_t*>(n, 0x38);
  if (!d) return;
  const uint8_t flags = At<uint8_t>(d, 0x121);
  if ((flags & 1) && !(flags & 8)) {  // collapsed: whole mesh
    ++g_st.collapsed;
    if (At<int16_t>(d, 0x13C) == 0) {
      ++g_st.holes_collapsed;
      if (g_hole_logs < kMaxHoleLogs) {
        ++g_hole_logs;
        LogNode("collapsed node with no mesh", n, -1);
      }
    }
    return;
  }
  const uint8_t* const* kids = At<const uint8_t* const*>(n, 4);
  const bool use_kids = kids && !(flags & 1);
  const uint16_t drawn = At<uint16_t>(d, 0x178);
  for (int q = 0; q < 4; ++q) {
    if (!(vis & (2u << q))) continue;
    for (int s = q * 4; s < q * 4 + 4; ++s) {
      if (!(vis & (0x20u << s))) continue;
      const int e = kSubEntry[s];
      const int nv = SubVerts(d, e);
      if (nv > 0) {
        if (drawn & (1u << e)) ++g_st.subs_drawn;
        else ++g_st.subs_culled;
      }
      if (!use_kids) {
        if (nv == 0) {
          ++g_st.holes_sub;
          if (g_hole_logs < kMaxHoleLogs) {
            ++g_hole_logs;
            LogNode("empty sub-block, no children", n, e);
          }
        }
        continue;
      }
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          const uint8_t* k = kids[kSubChild[s] + r * 16 + c];
          if (!k) {
            if (nv == 0) {
              ++g_st.holes_child;
              if (g_hole_logs < kMaxHoleLogs) {
                ++g_hole_logs;
                LogNode("empty sub-block, child missing", n, e);
              }
            }
            continue;
          }
          if (At<uint32_t>(k, 0x40)) ++g_st.children_drawn;
          Walk(k, depth + 1);
        }
      }
    }
  }
}

// Ctrl+F11: every visible node, one line each, and the camera.
void DumpNode(const uint8_t* n, int depth, unsigned& lines) {
  if (!n || depth > 12 || lines > 30000) return;
  const uint32_t vis = At<uint32_t>(n, 0x40);
  const uint8_t* d = At<const uint8_t*>(n, 0x38);
  const float* mn = reinterpret_cast<const float*>(n + 0x44);
  const float* mx = reinterpret_cast<const float*>(n + 0x50);
  char subs[16 * 7 + 1] = {};
  if (d && At<const uint8_t*>(d, 0x17C)) {
    char* p = subs;
    for (int e = 0; e < 16; ++e) p += wsprintfA(p, "%d,", SubVerts(d, e));
  }
  ++lines;
  patch::Log("tdump %*s%p L%u @%u,%u vis %06x fl %02x v %d dr %04x kids %d tex %d box %.0f %.0f %.0f %.0f %.0f %.0f "
             "subs %s",
             depth, "", n, At<uint16_t>(n, 0x20), At<uint16_t>(n, 0x24), At<uint16_t>(n, 0x26), vis,
             d ? At<uint8_t>(d, 0x121) : 0xFF, d ? At<int16_t>(d, 0x13C) : -1, d ? At<uint16_t>(d, 0x178) : 0,
             At<void*>(n, 4) ? 1 : 0, (At<void*>(n, 0x3C) && At<void*>(At<uint8_t*>(n, 0x3C), 8)) ? 1 : 0, mn[0],
             mn[1], mn[2], mx[0], mx[1], mx[2], subs);
  if (!vis || !d) return;
  const uint8_t* const* kids = At<const uint8_t* const*>(n, 4);
  if (!kids || (At<uint8_t>(d, 0x121) & 1)) return;
  for (int i = 0; i < 256; ++i)
    if (kids[i]) DumpNode(kids[i], depth + 1, lines);
}

void Dump(const uint8_t* root) {
  const uint8_t* terrain = At<const uint8_t*>(root, 0x34);
  if (terrain) {
    const uint8_t* cam = At<const uint8_t*>(terrain, 0xC8C);
    const float scale = At<float>(terrain, 0x40);
    patch::Log("tdump terrain %p scale %.3f tolerance %d far-colour detail %d/%d", terrain, scale,
               At<int>(terrain, 0xBEC), At<int>(terrain, 0xCA4), At<int>(terrain, 0xCA8));
    if (cam) {
      const float* p = reinterpret_cast<const float*>(cam + 0x170);
      const float* m = reinterpret_cast<const float*>(cam + 0xEC);
      patch::Log("tdump camera %p pos %.1f %.1f %.1f", cam, p[0], p[1], p[2]);
      for (int r = 0; r < 4; ++r)
        patch::Log("tdump matrix %.5f %.5f %.5f %.5f", m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
    }
  }
  patch::Log("tdump culling: sub-blocks %s, nodes %s", g_cull_sub_off ? "OFF" : "on", g_cull_node_off ? "OFF" : "on");
  unsigned lines = 0;
  DumpNode(root, 0, lines);
  patch::Log("tdump end: %u nodes", lines);
}

bool KeyPressed(int vk, bool& was) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool hit = down && !was;
  was = down;
  return hit;
}

bool OurWindowInFront() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

void Keys(const uint8_t* root) {
  static bool f9 = false, f10 = false, f11 = false;
  const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && OurWindowInFront();
  const bool k9 = KeyPressed(VK_F9, f9), k10 = KeyPressed(VK_F10, f10), k11 = KeyPressed(VK_F11, f11);
  if (!ctrl) return;
  if (k9) {
    g_cull_sub_off = !g_cull_sub_off;
    patch::Log("terrain: sub-block culling %s (Ctrl+F9)", g_cull_sub_off ? "OFF" : "on");
  }
  if (k10) {
    g_cull_node_off = !g_cull_node_off;
    patch::Log("terrain: node culling %s (Ctrl+F10)", g_cull_node_off ? "OFF" : "on");
  }
  if (k11) Dump(root);
}

void Report() {
  const DWORD now = GetTickCount();
  if (!g_st.start) g_st.start = now;
  if (now - g_st.start < 5000 || !g_st.frames) return;
  const double f = g_st.frames;
  patch::Log(
      "terrain 5s: %u frames; per frame: nodes %.0f visible %.0f (max %u) collapsed %.0f, sub-blocks drawn %.0f "
      "culled %.0f, children %.0f, meshed %.1f (max %u); render step %.3f ms (max %.2f); vertex buffers %d "
      "(%d KB); holes: collapsed %u, sub-block %u, child %u; culling sub %s node %s",
      g_st.frames, g_st.nodes / f, g_st.visible / f, g_st.max_visible, g_st.collapsed / f, g_st.subs_drawn / f,
      g_st.subs_culled / f, g_st.children_drawn / f, g_st.meshes / f, g_st.max_meshes, g_st.render_ms / f,
      g_st.max_render_ms, *reinterpret_cast<int*>(kVbCount), *reinterpret_cast<int*>(kVbBytes) / 1024,
      g_st.holes_collapsed, g_st.holes_sub, g_st.holes_child, g_cull_sub_off ? "OFF" : "on",
      g_cull_node_off ? "OFF" : "on");
  g_st = Stats();
  g_st.start = now;
}

int __fastcall RenderHook(void* root, void*) {
  LARGE_INTEGER t0, t1, fq;
  QueryPerformanceCounter(&t0);
  const int r = reinterpret_cast<ThisVoid>(kRender)(root, nullptr);
  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&fq);
  const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(fq.QuadPart);
  const auto* n = static_cast<const uint8_t*>(root);
  ++g_st.frames;
  g_st.render_ms += ms;
  if (ms > g_st.max_render_ms) g_st.max_render_ms = ms;
  const unsigned meshed = *reinterpret_cast<unsigned*>(kMeshCount);
  g_st.meshes += meshed;
  if (meshed > g_st.max_meshes) g_st.max_meshes = meshed;
  const unsigned long long before = g_st.visible;
  Walk(n, 0);
  const unsigned vis = static_cast<unsigned>(g_st.visible - before);
  if (vis > g_st.max_visible) g_st.max_visible = vis;
  Keys(n);
  Report();
  (void)r;
  return r;
}

int __fastcall CullNode(void* self, void* edx, void* a1, void* a2, void* a3, void* a4, int* a5, int* a6, void* a7) {
  if (g_cull_node_off) {
    if (a6) *a6 = 8;  // all eight box corners inside
    return 1;
  }
  return reinterpret_cast<CullFn>(kCull)(self, edx, a1, a2, a3, a4, a5, a6, a7);
}

int __fastcall CullSub(void* self, void* edx, void* a1, void* a2, void* a3, void* a4, int* a5, int* a6, void* a7) {
  if (g_cull_sub_off) {
    if (a6) *a6 = 8;
    return 1;
  }
  return reinterpret_cast<CullFn>(kCull)(self, edx, a1, a2, a3, a4, a5, a6, a7);
}

}  // namespace

void ApplyTerrainFixes() {
  const Config& c = GetConfig();
  if (!c.terrain_log) return;
  const uint8_t call_render[] = {0xE8, 0x4B, 0x50, 0xF7, 0xFF};  // call 4820e0
  const uint8_t call_cull_node[] = {0xE8, 0x2E, 0x1E, 0x0B, 0x00};  // call 534be0
  const uint8_t call_cull_sub[] = {0xE8, 0x79, 0x21, 0x0B, 0x00};   // call 534be0
  if (!patch::Matches(kRenderCall, call_render, 5) || !patch::Matches(kCullNodeCall, call_cull_node, 5) ||
      !patch::Matches(kCullSubCall, call_cull_sub, 5)) {
    patch::Log("terrain log: code differs, not applied");
    return;
  }
  patch::Call(kRenderCall, reinterpret_cast<const void*>(&RenderHook), 5);
  patch::Call(kCullNodeCall, reinterpret_cast<const void*>(&CullNode), 5);
  patch::Call(kCullSubCall, reinterpret_cast<const void*>(&CullSub), 5);
  patch::Log("terrain log: on (Ctrl+F9 sub-block culling, Ctrl+F10 node culling, Ctrl+F11 dump)");
}
