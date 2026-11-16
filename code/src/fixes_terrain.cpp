// Terrain: TerrainHoleFix (the game bug behind holes in distant terrain at
// long view distances), TerrainDetail, TerrainLodFix, FastTerrain's hooks
// and TerrainCheck (the rewrites themselves are in terrain_fast.cpp), the
// Profile=1 terrain zones, and TerrainLog diagnostics.
//
// TerrainHoleFix: see TerrainVisFix below. Children of a split node that
// Setup found wholly in view were never refreshed by Eval, so pieces that
// were off screen earlier stayed invisible: holes with sky behind them.
//
// TerrainLodFix: per node and frame, 0x482550 works out two detail numbers
// from the camera's distance to the node's box (data+0x122 and +0x124,
// 16 bits each; last frame's copies at +0x126 / +0x128). Eval (0x4836e0)
// turns them into vertex counts through the byte table 0x66b3ec. Most paths
// clamp the number at 65535 (compare with 65535.0 at 0x556148), but one
// (0x4827f8) converts with 0x53a0f8 and stores the low 16 bits; its clamp
// (0x482820, cmp word,0xffff / jbe) can never fire. The fix replaces that
// conversion call (0x48280f) with one that stops at 65535, as the other
// paths do. It was a suspect for the holes, but the number has not been seen
// to pass 65535 even at ViewDistance=10; kept as a safety net.
//
// The game's terrain (object at the terrain manager, per-frame update
// 0x50cdc0) is a tree of nodes. Each node is a 16x16-cell block with up to
// 256 children (node+4, a 16x16 array of finer nodes, loaded from the track
// file on demand by 0x483440). Per frame:
//
//  - Setup (0x482d20 -> 0x482d50): frustum test of each node's box
//    (+0x44 centre, +0x50 half size) through 0x534be0; node+0x40 holds the
//    visible parts: bit 0 the node, bits 1-4 its quadrants, bits 5-20 its
//    4x4 sub-blocks (Z order). Collapsed nodes (data+0x121 bit 0) are drawn
//    whole from their own mesh, otherwise their children are used.
//  - Eval (0x4836e0) and Mesh (0x47fec0): vertex activation and mesh builds
//    for changed blocks (data+0x121 bit 1).
//  - Render (0x4820e0): per visible sub-block a second frustum test
//    (0x482990, box from the sub-block table at data+0x17c) sets the bit in
//    data+0x178 the draw pass (0x4818b0 / 0x481a40 / 0x481c00) reads.
//
// Ctrl+F8 (FastTerrain=1, game window in front): FastTerrain on / off.
// Debug keys (TerrainLog=1, game window in front):
//   Ctrl+F9   TerrainHoleFix on / off
//   Ctrl+F10  TerrainDetail: ini value, x2, x4, x0.5, x0.25 of it (leaf culling
//             at 0x483281 is still counted; turning it off did not help)
// TerrainLog=2 also looks for holes every 2 seconds on its own: the terrain
// draws of one frame are rasterised at quarter size, and any pixel below the
// highest terrain pixel of its column that no terrain triangle covers is a
// hole (terrain is a height field); the nodes at the biggest hole are logged.
//   Ctrl+F11  write the terrain tree (visible nodes) and the camera to the log,
//             and probe the point under the mouse cursor: the next frames
//             log every draw with a triangle over it (dd/probe.cpp) and
//             every node whose box covers it
// Ruled out on the way (StOo's tests): culling, the far plane, the
// one-colour far terrain, the terrain caches' trim limits, re-meshing empty
// collapsed blocks (75% of collapsed blocks are empty normally), MCM2MaX's
// renderer (the gaps showed on Direct3D 9 with batching off too), the
// detail-number overflow, the detail level.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <cstdarg>
#include <vector>

#include "config.h"
#include "dd/gpushadow.h"
#include "freecam.h"
#include "dd/probe.h"
#include "dd/racetitle.h"
#include "patch.h"
#include "prof.h"
#include "simd.h"
#include "terrain_fast.h"
#include "fpu_guard.h"
#include "gputerrain.h"

// TerrainLodFix state, shared with the stub below (C names for the asm).
extern "C" {
unsigned g_lod_fix = 0;        // clamp at 65535 (Ctrl+F9 flips it with TerrainLog=1)
unsigned g_lod_calls = 0;      // conversions on the unclamped path
unsigned g_lod_over = 0;       // ... of which over 65535
unsigned g_lod_max = 0;        // largest value seen
unsigned g_lod_ring_i = 0;     // last 16 overflows: node grid x/y and level, and the value
unsigned g_lod_ring_node[16];
unsigned g_lod_ring_value[16];
long long g_lod_tmp = 0;
float g_lod_limit = 65535.0f;
void TerrainLodFtol();
}

#ifdef _MSC_VER
// Replaces "call 53a0f8" at 0x48280f: st0 = the detail number (popped),
// result in eax (edx high half, as 53a0f8 gives). esi = the node.
__declspec(naked) void TerrainLodFtol() {
  __asm {
    inc g_lod_calls
    fcom g_lod_limit
    fnstsw ax
    test ah, 41h
    jnz original
    fld st(0)
    fistp g_lod_tmp
    inc g_lod_over
    mov eax, dword ptr g_lod_tmp
    cmp eax, g_lod_max
    jbe ring
    mov g_lod_max, eax
  ring:
    mov ecx, g_lod_ring_i
    and ecx, 15
    mov g_lod_ring_value[ecx*4], eax
    movzx edx, word ptr [esi+24h]
    and edx, 0FFFh
    movzx eax, word ptr [esi+26h]
    and eax, 0FFFh
    shl eax, 12
    or edx, eax
    movzx eax, byte ptr [esi+20h]
    shl eax, 24
    or edx, eax
    mov g_lod_ring_node[ecx*4], edx
    inc g_lod_ring_i
    cmp g_lod_fix, 0
    je original
    fstp st(0)
    mov eax, 0FFFFh
    xor edx, edx
    ret
  original:
    mov eax, 53A0F8h
    jmp eax
  }
}
#else
asm(".text\n"
    ".globl _TerrainLodFtol\n"
    "_TerrainLodFtol:\n"
    "  incl _g_lod_calls\n"
    "  fcoms _g_lod_limit\n"
    "  fnstsw %ax\n"
    "  testb $0x41, %ah\n"
    "  jnz 2f\n"
    "  fld %st(0)\n"
    "  fistpll _g_lod_tmp\n"
    "  incl _g_lod_over\n"
    "  movl _g_lod_tmp, %eax\n"
    "  cmpl _g_lod_max, %eax\n"
    "  jbe 1f\n"
    "  movl %eax, _g_lod_max\n"
    "1:\n"
    "  movl _g_lod_ring_i, %ecx\n"
    "  andl $15, %ecx\n"
    "  movl %eax, _g_lod_ring_value(,%ecx,4)\n"
    "  movzwl 0x24(%esi), %edx\n"
    "  andl $0xFFF, %edx\n"
    "  movzwl 0x26(%esi), %eax\n"
    "  andl $0xFFF, %eax\n"
    "  shll $12, %eax\n"
    "  orl %eax, %edx\n"
    "  movzbl 0x20(%esi), %eax\n"
    "  shll $24, %eax\n"
    "  orl %eax, %edx\n"
    "  movl %edx, _g_lod_ring_node(,%ecx,4)\n"
    "  incl _g_lod_ring_i\n"
    "  cmpl $0, _g_lod_fix\n"
    "  je 2f\n"
    "  fstp %st(0)\n"
    "  movl $0xFFFF, %eax\n"
    "  xorl %edx, %edx\n"
    "  ret\n"
    "2:\n"
    "  movl $0x53A0F8, %eax\n"
    "  jmp *%eax\n");
#endif

// TerrainHoleFix. Eval (0x4836e0) walks every visible node that is split
// into children and, when the node is wholly in view, marks each child
// wholly in view too (vis 0x3fffff): "cmp [esi+40h],200000h / jne" at
// 0x48371d. But a node that Setup found wholly in view (0x4830c9) has vis
// 0x3fffff, not 0x200000, so the exact compare fails and its children keep
// whatever vis they had the last time the node was only partly in view;
// children that were off screen then (vis 0) are never built or drawn while
// the parent's own mesh leaves their area to them: a hole until the camera
// turns enough for the node to be partly in view again. The original view
// distance seldom has a split node wholly in view, ViewDistance=10 often
// does. The fix tests the bit instead (0x200000 set) for 0x3fffff nodes.
extern "C" {
unsigned g_hole_fix = 1;
unsigned g_hole_fixed = 0;     // children whose vis the fix changed
unsigned g_hole_parents = 0;   // wholly-in-view split nodes seen
void __cdecl TerrainVisFix(uint8_t* parent, uint8_t* child) {
  uint32_t v, c;
  std::memcpy(&v, parent + 0x40, 4);
  if (v == 0x200000) {
    c = 0x3FFFFF;
    std::memcpy(child + 0x40, &c, 4);
    return;
  }
  if (v != 0x3FFFFF) return;
  ++g_hole_parents;
  std::memcpy(&c, child + 0x40, 4);
  if (c == 0x3FFFFF) return;
  ++g_hole_fixed;
  if (!g_hole_fix) return;
  c = 0x3FFFFF;
  std::memcpy(child + 0x40, &c, 4);
}
void TerrainVisStub();
}

#ifdef _MSC_VER
// Replaces "cmp [esi+40h],ebp / jne 483725" at 0x48371d (eax = the child,
// esi = the node); goes on at 0x483725, which reloads ecx and edx.
__declspec(naked) void TerrainVisStub() {
  __asm {
    push eax
    push esi
    call TerrainVisFix
    add esp, 8
    mov eax, 483725h
    jmp eax
  }
}
#else
asm(".text\n"
    ".globl _TerrainVisStub\n"
    "_TerrainVisStub:\n"
    "  pushl %eax\n"
    "  pushl %esi\n"
    "  call _TerrainVisFix\n"
    "  addl $8, %esp\n"
    "  movl $0x483725, %eax\n"
    "  jmp *%eax\n");
#endif

namespace {

constexpr uintptr_t kRenderCall = 0x50D090;  // call 4820e0 (terrain render step), ecx = root node
constexpr uintptr_t kRender = 0x4820E0;
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

template <typename T>
T At(const void* p, size_t off) {
  T v;
  std::memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof(T));
  return v;
}

// Terrain detail (TerrainDetail): the terrain picks how finely to mesh each
// block from a screen-space error: the detail level's tolerance (terrain+0x6c,
// pixels: 1 at the best level, from the table at [0x69046c]) against the
// camera's focal length (+0x70, 655 at 1080p). 0x50d350 (called every frame
// at 0x50cff3) turns the two into the factors at +0x74..+0x80 that the detail
// numbers (0x482550) use. TerrainDetail multiplies the focal length for that
// call only, so the blocks are built as if seen TerrainDetail times bigger
// (more detail above 1, less below).
constexpr uintptr_t kParamsCall = 0x50CFF3;
constexpr uintptr_t kHoleCmp = 0x48371D;
constexpr uintptr_t kParams = 0x50D350;
float g_detail = 1.0f;
float g_detail_ini = 1.0f;
bool g_params_hooked = false;
bool g_hole_hooked = false;
int g_last_tol = 0, g_last_focal = 0;

void __fastcall ParamsHook(uint8_t* t, void*) {
  const int focal = At<int>(t, 0x70);
  g_last_tol = At<int>(t, 0x6C);
  g_last_focal = focal;
  int scaled = static_cast<int>(focal * g_detail + 0.5f);
  if (scaled < 1) scaled = 1;
  std::memcpy(t + 0x70, &scaled, 4);
  reinterpret_cast<ThisVoid>(kParams)(t, nullptr);
  std::memcpy(t + 0x70, &focal, 4);
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

// Leaf culling (Setup 0x482d50, call at 0x483281): a node with no children
// of its own is frustum-tested on its own box when its parent's region is
// only partly in view. TerrainLog counts the tests (turning the test off did
// not fill the holes).
constexpr uintptr_t kLeafCullCall = 0x483281;
constexpr uintptr_t kFrustumTest = 0x534BE0;
using FrustumFn = int(__fastcall*)(void* self, void* edx, void* m, void* cam, const float* c, const float* h, int a5,
                                   int a6, int* inside);
unsigned g_leaf_tests = 0, g_leaf_culled = 0;

int __fastcall LeafCull(void* self, void* edx, void* m, void* cam, const float* c, const float* h, int a5, int a6,
                        int* inside) {
  const int r = reinterpret_cast<FrustumFn>(kFrustumTest)(self, edx, m, cam, c, h, a5, a6, inside);
  ++g_leaf_tests;
  if (!r) ++g_leaf_culled;
  return r;
}

bool g_lod_hooked = false;

// The last overflows (up to 16) since the previous report.
void ReportOverNodes() {
  const unsigned n = g_lod_ring_i < 16 ? g_lod_ring_i : 16;
  if (!n) return;
  char buf[16 * 32 + 1] = {};
  char* p = buf;
  for (unsigned i = 0; i < n; ++i) {
    const unsigned v = g_lod_ring_node[i];
    p += wsprintfA(p, " L%u@%u,%u=%u", v >> 24, v & 0xFFF, (v >> 12) & 0xFFF, g_lod_ring_value[i]);
  }
  patch::Log("terrain: detail number overflows (level@x,z=value):%s", buf);
  g_lod_ring_i = 0;
}

unsigned g_hole_logs = 0;

constexpr uintptr_t kLodCall = 0x48280F;  // call 53a0f8 (float to int), result stored as a word at 0x482814
constexpr uintptr_t kFtol = 0x53A0F8;  // detailed hole lines written (capped)
constexpr unsigned kMaxHoleLogs = 0;  // the hole check misreads leaf nodes; counts only

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


// Ctrl+F11 probe: every node (visible or not, at any depth) whose box covers
// the probed point, with which of its sub-blocks do.
struct ProbeCtx {
  float m[16];
  float nx, ny;
  unsigned lines;
};

// Does the box (centre c, half size h) cover the point on screen?
// 2 = yes, with a corner behind the camera (so not certain), 1 = yes, 0 = no.
int BoxCovers(const ProbeCtx& p, const float* c, const float* h, float* depth) {
  float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, wmin = 1e30f;
  bool behind = false;
  for (int i = 0; i < 8; ++i) {
    const float q[3] = {c[0] + (i & 1 ? h[0] : -h[0]), c[1] + (i & 2 ? h[1] : -h[1]), c[2] + (i & 4 ? h[2] : -h[2])};
    const float* m = p.m;
    const float x = q[0] * m[0] + q[1] * m[4] + q[2] * m[8] + m[12];
    const float y = q[0] * m[1] + q[1] * m[5] + q[2] * m[9] + m[13];
    const float w = q[0] * m[3] + q[1] * m[7] + q[2] * m[11] + m[15];
    if (w <= 1e-3f) {
      behind = true;
      continue;
    }
    if (w < wmin) wmin = w;
    const float sx = x / w, sy = y / w;
    if (sx < x0) x0 = sx;
    if (sx > x1) x1 = sx;
    if (sy < y0) y0 = sy;
    if (sy > y1) y1 = sy;
  }
  *depth = wmin;
  if (wmin > 1e29f) return 0;  // all behind the camera
  if (behind) return 2;
  return (p.nx >= x0 && p.nx <= x1 && p.ny >= y0 && p.ny <= y1) ? 1 : 0;
}

void ProbeNode(ProbeCtx& p, const uint8_t* n, int depth) {
  if (!n || depth > 12 || p.lines >= 400) return;
  const float* c = reinterpret_cast<const float*>(n + 0x44);
  const float* h = reinterpret_cast<const float*>(n + 0x50);
  float dist = 0;
  const int cov = BoxCovers(p, c, h, &dist);
  if (!cov) return;
  const uint8_t* d = At<const uint8_t*>(n, 0x38);
  // Sub-blocks over the point (row-major e = row * 4 + col; col along x,
  // row along z; y from the sub-block table).
  char subs[16 * 16 + 1] = {};
  char* sp = subs;
  const uint8_t* t = d ? At<const uint8_t*>(d, 0x17C) : nullptr;
  if (t && cov == 1) {
    for (int e = 0; e < 16; ++e) {
      const float sc[3] = {c[0] - h[0] + (e % 4 + 0.5f) * h[0] / 2, At<float>(t, e * 12 + 4),
                           c[2] - h[2] + (e / 4 + 0.5f) * h[2] / 2};
      const float sh[3] = {h[0] / 4, At<float>(t, e * 12 + 8), h[2] / 4};
      float sd = 0;
      if (BoxCovers(p, sc, sh, &sd)) sp += wsprintfA(sp, "%d:%d ", e, SubVerts(d, e));
    }
  }
  // Nodes holding the camera (partly behind it) are followed but only
  // logged near the top of the tree.
  if (cov == 2 && depth > 1) goto children;
  ++p.lines;
  patch::Log("probe node %*s%p L%u @%u,%u %s dist %.0f vis %06x fl %02x v %d dr %04x kids %d tex %d box %.0f %.0f %.0f "
             "+- %.0f %.0f %.0f; subs over point (e:verts) %s",
             depth, "", n, At<uint16_t>(n, 0x20), At<uint16_t>(n, 0x24), At<uint16_t>(n, 0x26),
             cov == 2 ? "PARTLY-BEHIND" : "covers", dist, At<uint32_t>(n, 0x40), d ? At<uint8_t>(d, 0x121) : 0xFF,
             d ? At<int16_t>(d, 0x13C) : -1, d ? At<uint16_t>(d, 0x178) : 0, At<void*>(n, 4) ? 1 : 0,
             (At<void*>(n, 0x3C) && At<void*>(At<uint8_t*>(n, 0x3C), 8)) ? 1 : 0, c[0], c[1], c[2], h[0], h[1], h[2],
             subs);
children:
  const uint8_t* const* kids = At<const uint8_t* const*>(n, 4);
  if (!kids) return;
  for (int i = 0; i < 256; ++i)
    if (kids[i]) ProbeNode(p, kids[i], depth + 1);
}

volatile bool g_dump_pending = false;

int g_probe_walked_stage = 0;

void ProbeTerrain(const uint8_t* root) {
  const int st = dd::ProbeStage();
  if (st != 2 && st != 3) {
    g_probe_walked_stage = 0;
    return;
  }
  if (g_probe_walked_stage == st) return;
  g_probe_walked_stage = st;
  if (st == 2 && !dd::ProbeLogsPoint()) return;  // automatic search: only the hole's nodes
  ProbeCtx p{};
  if (!dd::ProbeTerrainView(p.m, &p.nx, &p.ny)) return;
  patch::Log("probe: terrain nodes over the %s (any state):", st == 3 ? "biggest hole" : "point");
  ProbeNode(p, root, 0);
  patch::Log("probe: %u node lines%s", p.lines, p.lines >= 400 ? " (cut at 400)" : "");
}

void ArmProbe() {
  POINT pt;
  HWND w = GetForegroundWindow();
  RECT rc;
  if (!GetCursorPos(&pt) || !ScreenToClient(w, &pt) || !GetClientRect(w, &rc) || rc.right <= 0 || rc.bottom <= 0) {
    patch::Log("probe: no cursor position");
    return;
  }
  patch::Log("probe: cursor at %ld,%ld of %ldx%ld", static_cast<long>(pt.x), static_cast<long>(pt.y),
             static_cast<long>(rc.right), static_cast<long>(rc.bottom));
  dd::ProbeArm(2.0f * (pt.x + 0.5f) / rc.right - 1.0f, 1.0f - 2.0f * (pt.y + 0.5f) / rc.bottom);
}

bool KeyPressed(int vk, bool& was);
bool OurWindowInFront();

// Ctrl+F11 is watched on its own thread: the terrain is not drawn while the
// game is paused, and pausing is how the mouse cursor gets over a gap.
DWORD WINAPI KeyThread(void*) {
  bool f11 = false;
  DWORD last_auto = GetTickCount();
  const bool automatic = GetConfig().terrain_log >= 2;
  for (;;) {
    Sleep(15);
    // TerrainLog=2: look for holes every 2 seconds (logged only when found).
    if (automatic && GetTickCount() - last_auto >= 2000) {
      last_auto = GetTickCount();
      if (dd::ProbeStage() == 0 && OurWindowInFront()) dd::ProbeArm(0, 0, false);
    }
    const bool hit = KeyPressed(VK_F11, f11);
    if (hit && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && OurWindowInFront()) {
      ArmProbe();
      g_dump_pending = true;
    }
  }
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
  static bool f9 = false;
  const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && OurWindowInFront();
  const bool k9 = KeyPressed(VK_F9, f9);
  static bool f10 = false;
  if (KeyPressed(VK_F10, f10) && ctrl) {
    if (!g_params_hooked) {
      patch::Log("terrain: detail hook not installed (code differs); Ctrl+F10 does nothing");
    } else {
      // ini value, then x2, x4, x0.5, x0.25 of it, then back.
      static const float kSteps[] = {1.0f, 2.0f, 4.0f, 0.5f, 0.25f};
      static int step = 0;
      step = (step + 1) % 5;
      g_detail = g_detail_ini * kSteps[step];
      patch::Log("terrain: TerrainDetail now %.2f (Ctrl+F10; ini %.2f)", g_detail, g_detail_ini);
      char msg[64];
      std::snprintf(msg, sizeof(msg), "TerrainDetail %.2f", g_detail);
      dd::DebugNotice(msg);
    }
  }
  if (ctrl && k9) {
    if (!g_hole_hooked) {
      patch::Log("terrain: TerrainHoleFix hook not installed (code differs); Ctrl+F9 does nothing");
    } else {
      g_hole_fix = !g_hole_fix;
      patch::Log("terrain: TerrainHoleFix %s (Ctrl+F9)", g_hole_fix ? "ON" : "off");
      dd::DebugNotice(g_hole_fix ? "TerrainHoleFix ON" : "TerrainHoleFix off");
    }
  }
  if (g_dump_pending) {
    g_dump_pending = false;
    Dump(root);
  }
}

void Report() {
  const DWORD now = GetTickCount();
  if (!g_st.start) g_st.start = now;
  if (now - g_st.start < 5000 || !g_st.frames) return;
  const double f = g_st.frames;
  patch::Log(
      "terrain 5s: %u frames; per frame: nodes %.0f visible %.0f (max %u) collapsed %.0f, sub-blocks drawn %.0f "
      "culled %.0f, children %.0f, meshed %.1f (max %u); render step %.3f ms (max %.2f); vertex buffers %d "
      "(%d KB); empty collapsed blocks %.0f; detail number: %u conversions on the unclamped path, %u over 65535 "
      "(largest %u), TerrainLodFix %s",
      g_st.frames, g_st.nodes / f, g_st.visible / f, g_st.max_visible, g_st.collapsed / f, g_st.subs_drawn / f,
      g_st.subs_culled / f, g_st.children_drawn / f, g_st.meshes / f, g_st.max_meshes, g_st.render_ms / f,
      g_st.max_render_ms, *reinterpret_cast<int*>(kVbCount), *reinterpret_cast<int*>(kVbBytes) / 1024,
      g_st.holes_collapsed / f, g_lod_calls, g_lod_over, g_lod_max, !g_lod_hooked ? "not installed" : g_lod_fix ? "on" : "off");
  patch::Log("terrain 5s: leaf tests %.0f culled %.0f; tolerance %d focal %d TerrainDetail %.2f; TerrainHoleFix %s: "
             "wholly-in-view split nodes %.1f, children it marked visible %.1f",
             g_leaf_tests / f, g_leaf_culled / f, g_last_tol, g_last_focal, g_detail,
             !g_hole_hooked ? "not installed" : g_hole_fix ? "on" : "off", g_hole_parents / f, g_hole_fixed / f);
  g_hole_parents = g_hole_fixed = 0;
  g_leaf_tests = g_leaf_culled = 0;
  ReportOverNodes();
  g_lod_calls = g_lod_over = g_lod_max = 0;
  g_st = Stats();
  g_st.start = now;
}

int __fastcall BlocksZone(void* root, void*);

int __fastcall RenderHook(void* root, void*) {
  LARGE_INTEGER t0, t1, fq;
  QueryPerformanceCounter(&t0);
  const int r = BlocksZone(root, nullptr);
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
  ProbeTerrain(n);
  Report();
  (void)r;
  return r;
}


// ---- FastTerrain (terrain_fast.cpp) ----------------------------------------
//
// The game's Detail (0x482550), Eval (0x4836e0) and draw walk (0x4818b0)
// entry points jump to the exact rewrites. TerrainCheck=1: every 2 seconds
// one frame runs each of them twice, the game's own code first (state saved
// and put back), and compares: Detail's return value and bytes per call, all
// node and terrain bytes after Eval, the list of draw calls of the walk.
// Ctrl+F8 switches FastTerrain on and off while racing (shown on screen).
constexpr uintptr_t kDetailFn = 0x482550;
constexpr uintptr_t kEvalFn = 0x4836E0;
constexpr uintptr_t kDrawWalkFn = 0x4818B0;
constexpr uintptr_t kDrawFn = 0x481C00;
constexpr uintptr_t kDrawAltFn = 0x481F20;
constexpr uintptr_t kMeshTriFn = 0x480C30;
constexpr uintptr_t kMeshCornerFn = 0x481640;

struct Hook {
  uintptr_t at = 0;
  uint8_t orig[6] = {};
  uint8_t ours[6] = {};
  bool in = false;
  void Install(const void* target) {
    std::memcpy(orig, reinterpret_cast<const void*>(at), 6);
    patch::Jump(at, reinterpret_cast<uintptr_t>(target), 6);
    std::memcpy(ours, reinterpret_cast<const void*>(at), 6);
    in = true;
  }
  void Off() {
    if (in) patch::Write(at, orig, 6);
    in = false;
  }
  void On() {
    if (!in) patch::Write(at, ours, 6);
    in = true;
  }
};
Hook g_h_detail, g_h_eval, g_h_draw, g_h_tri, g_h_corner;
bool g_fast_hooked = false;
bool g_fast_on = false;
uint8_t* g_detail_tramp = nullptr;  // the game's Detail: its first 6 bytes, then jmp 0x482556

using DetailFn = float(__fastcall*)(void* n, void* edx, int flag);
using EvalFn = void(__fastcall*)(void* n, void* edx);
using DrawWalkFn = int(__fastcall*)(void* n, void* edx, int a1, int x, int y, int size, int q, int a6);
using MeshTriFn = int(__fastcall*)(void* n, void* edx, int L, int cnt, int x, int y, int dx, int dy, int* out);
using MeshCornerFn = int(__fastcall*)(void* n, void* edx, int t, int x, int y, int dx, int dy);

struct CheckStats {
  unsigned detail = 0, detail_bad = 0;
  unsigned eval = 0, eval_bad_nodes = 0, eval_nodes = 0, eval_bad_terrain = 0;
  unsigned draw = 0, draw_calls = 0, draw_bad = 0;
  unsigned mesh = 0, mesh_bad = 0;
  unsigned logged = 0;
  double eval_ms = 0;
} g_chk;
bool g_check_detail = false;  // this frame's Setup compares Detail
bool g_check_draw = false;    // this frame's draw walk is compared
bool g_check_mesh = false;    // this frame's mesh building is compared
DWORD g_check_last = 0;

void SyncFastOptions() {
  tf::g_opt.lod_fix = g_lod_hooked && g_lod_fix;
  tf::g_opt.hole_fix_hooked = g_hole_hooked;
  tf::g_opt.hole_fix = g_hole_fix != 0;
}

void CheckLog(const char* fmt, ...) {
  if (g_chk.logged >= 40) return;
  ++g_chk.logged;
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  patch::Log("terrain check: %s%s", buf, g_chk.logged == 40 ? " (no more mismatch lines)" : "");
}

float CheckDetail(uint8_t* n, int flag) {
  uint8_t* d = At<uint8_t*>(n, 0x38);
  uint8_t* t = At<uint8_t*>(n, 0x34);
  uint8_t sd[8], st[4], gd[8], gt[4];
  std::memcpy(sd, d + 0x122, 8);
  std::memcpy(st, t + 0x98, 4);
  const float rg = reinterpret_cast<DetailFn>(g_detail_tramp)(n, nullptr, flag);
  std::memcpy(gd, d + 0x122, 8);
  std::memcpy(gt, t + 0x98, 4);
  std::memcpy(d + 0x122, sd, 8);
  std::memcpy(t + 0x98, st, 4);
  const float ro = tf::Detail(reinterpret_cast<tf::Node*>(n), flag);
  ++g_chk.detail;
  if (std::memcmp(&rg, &ro, 4) != 0 || std::memcmp(gd, d + 0x122, 8) != 0 || std::memcmp(gt, t + 0x98, 4) != 0) {
    ++g_chk.detail_bad;
    CheckLog("Detail differs: node L%u @%u,%u flag %d: return %.9g / %.9g, lo/hi %u/%u ours %u/%u", At<uint8_t>(n, 0x28),
             At<uint16_t>(n, 0x24), At<uint16_t>(n, 0x26), flag, rg, ro, At<uint16_t>(gd, 0), At<uint16_t>(gd, 2),
             At<uint16_t>(d, 0x122), At<uint16_t>(d, 0x124));
  }
  return ro;
}

float __fastcall DetailHook(uint8_t* n, void*, int flag) {
  if (g_check_detail) return CheckDetail(n, flag);
  return tf::Detail(reinterpret_cast<tf::Node*>(n), flag);
}

// Every node of the tree (any state).
void CollectNodes(uint8_t* n, std::vector<uint8_t*>& out, int depth) {
  if (!n || depth > 12) return;
  out.push_back(n);
  uint8_t** kids = At<uint8_t**>(n, 4);
  if (!kids) return;
  for (int i = 0; i < 256; ++i)
    if (kids[i]) CollectNodes(kids[i], out, depth + 1);
}

constexpr size_t kNodeSnap = 4 + 0x134;  // vis + data bytes 0..0x133
constexpr size_t kTerrSnapOff = 0x90, kTerrSnapLen = 0x540 - 0x90;

void Snap(const std::vector<uint8_t*>& nodes, const uint8_t* t, std::vector<uint8_t>& buf) {
  buf.resize(nodes.size() * kNodeSnap + kTerrSnapLen);
  uint8_t* p = buf.data();
  for (uint8_t* n : nodes) {
    std::memcpy(p, n + 0x40, 4);
    std::memcpy(p + 4, At<uint8_t*>(n, 0x38), 0x134);
    p += kNodeSnap;
  }
  std::memcpy(p, t + kTerrSnapOff, kTerrSnapLen);
}

void Restore(const std::vector<uint8_t*>& nodes, uint8_t* t, const std::vector<uint8_t>& buf) {
  const uint8_t* p = buf.data();
  for (uint8_t* n : nodes) {
    std::memcpy(n + 0x40, p, 4);
    std::memcpy(At<uint8_t*>(n, 0x38), p + 4, 0x134);
    p += kNodeSnap;
  }
  std::memcpy(t + kTerrSnapOff, p, kTerrSnapLen);
}

void CheckEval(uint8_t* root) {
  LARGE_INTEGER t0, t1, fq;
  QueryPerformanceCounter(&t0);
  static std::vector<uint8_t*> nodes;
  static std::vector<uint8_t> pre, game, ours;
  nodes.clear();
  CollectNodes(root, nodes, 0);
  uint8_t* t = At<uint8_t*>(root, 0x34);
  Snap(nodes, t, pre);
  g_h_eval.Off();
  reinterpret_cast<EvalFn>(kEvalFn)(root, nullptr);
  g_h_eval.On();
  Snap(nodes, t, game);
  Restore(nodes, t, pre);
  tf::Eval(reinterpret_cast<tf::Node*>(root));
  Snap(nodes, t, ours);
  ++g_chk.eval;
  g_chk.eval_nodes += static_cast<unsigned>(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) {
    const uint8_t* a = game.data() + i * kNodeSnap;
    const uint8_t* b = ours.data() + i * kNodeSnap;
    if (std::memcmp(a, b, kNodeSnap) == 0) continue;
    ++g_chk.eval_bad_nodes;
    size_t o = 0;
    while (a[o] == b[o]) ++o;
    const uint8_t* n = nodes[i];
    CheckLog("Eval differs: node L%u @%u,%u %s+%03x game %02x ours %02x", At<uint8_t>(n, 0x28), At<uint16_t>(n, 0x24),
             At<uint16_t>(n, 0x26), o < 4 ? "vis" : "data", static_cast<unsigned>(o < 4 ? o : o - 4), a[o], b[o]);
  }
  const uint8_t* ga = game.data() + nodes.size() * kNodeSnap;
  const uint8_t* ob = ours.data() + nodes.size() * kNodeSnap;
  if (std::memcmp(ga, ob, kTerrSnapLen) != 0) {
    ++g_chk.eval_bad_terrain;
    size_t o = 0;
    while (ga[o] == ob[o]) ++o;
    CheckLog("Eval differs: terrain +%03x game %02x ours %02x", static_cast<unsigned>(o + kTerrSnapOff), ga[o], ob[o]);
  }
  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&fq);
  g_chk.eval_ms += 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(fq.QuadPart);
}

struct DrawRec {
  int fn;
  void* n;
  int a1;
  void* verts;
  int vc;
  void* idx;
  int ic;
  int sub;
  bool operator==(const DrawRec& o) const {
    return fn == o.fn && n == o.n && a1 == o.a1 && verts == o.verts && vc == o.vc && idx == o.idx && ic == o.ic &&
           sub == o.sub;
  }
};
std::vector<DrawRec> g_rec;
int __fastcall RecDraw(void* n, void*, int a1, void* v, int vc, void* idx, int ic, int sub) {
  g_rec.push_back({0, n, a1, v, vc, idx, ic, sub});
  return 1;
}
int __fastcall RecDrawAlt(void* n, void*, int a1, void* v, int vc, void* idx, int ic, int sub) {
  g_rec.push_back({1, n, a1, v, vc, idx, ic, sub});
  return 1;
}

void CheckDraw(uint8_t* n, int a1, int x, int y, int size, int q, int a6) {
  Hook r1, r2;
  r1.at = kDrawFn;
  r2.at = kDrawAltFn;
  r1.Install(reinterpret_cast<const void*>(&RecDraw));
  r2.Install(reinterpret_cast<const void*>(&RecDrawAlt));
  g_rec.clear();
  g_h_draw.Off();
  reinterpret_cast<DrawWalkFn>(kDrawWalkFn)(n, nullptr, a1, x, y, size, q, a6);
  g_h_draw.On();
  std::vector<DrawRec> game;
  game.swap(g_rec);
  tf::DrawWalk(reinterpret_cast<tf::Node*>(n), a1, x, y, size, q, a6);
  r1.Off();
  r2.Off();
  ++g_chk.draw;
  g_chk.draw_calls += static_cast<unsigned>(game.size());
  if (!(game == g_rec)) {
    ++g_chk.draw_bad;
    size_t i = 0;
    while (i < game.size() && i < g_rec.size() && game[i] == g_rec[i]) ++i;
    CheckLog("draw walk differs: game %u calls, ours %u, first difference at %u", static_cast<unsigned>(game.size()),
             static_cast<unsigned>(g_rec.size()), static_cast<unsigned>(i));
  }
  g_rec.clear();
}

int __fastcall DrawHook(uint8_t* n, void*, int a1, int x, int y, int size, int q, int a6) {
  prof::Scope z(prof::kTerrDraw);
  g_check_mesh = false;
  if (g_check_draw) {
    g_check_draw = false;
    CheckDraw(n, a1, x, y, size, q, a6);
  }
  return tf::DrawWalk(reinterpret_cast<tf::Node*>(n), a1, x, y, size, q, a6);
}

// Mesh building: Tri (0x480c30) and Corner (0x481640) write vertices into
// the scratch builder 0x67da58 and indices from 0x6807d4. The check runs the
// game's call first, then ours from the same start, and compares the
// builder, the indices written, the return value and the flag.
constexpr uintptr_t kBuilder = 0x67DA58;
constexpr size_t kBuilderLen = 0x6807D4 - 0x67DA58;  // up to the index array
constexpr uintptr_t kMeshIdx = 0x6807D4;
constexpr size_t kMeshIdxMax = 3 * 1100;  // triangles of one node mesh, with room

void CheckMeshResult(const char* what, uint8_t* n, int rg, int ro, int og, int oo, int t0,
                     const std::vector<uint8_t>& gb, const std::vector<uint16_t>& gi) {
  ++g_chk.mesh;
  const uint8_t* b = reinterpret_cast<const uint8_t*>(kBuilder);
  const uint16_t* ix = reinterpret_cast<const uint16_t*>(kMeshIdx);
  bool bad = rg != ro || og != oo;
  size_t o = kBuilderLen;
  if (!bad && std::memcmp(gb.data(), b, kBuilderLen) != 0) {
    o = 0;
    while (gb[o] == b[o]) ++o;
    bad = true;
  }
  int ti = -1;
  if (!bad && rg > t0)
    for (int i = t0 * 3; i < rg * 3 && i < static_cast<int>(kMeshIdxMax); ++i)
      if (gi[i] != ix[i]) {
        ti = i;
        bad = true;
        break;
      }
  if (!bad) return;
  ++g_chk.mesh_bad;
  CheckLog("%s differs: node L%u @%u,%u: triangles %d / %d, flag %d / %d, builder +%04x %s, index %d", what,
           At<uint8_t>(n, 0x28), At<uint16_t>(n, 0x24), At<uint16_t>(n, 0x26), rg, ro, og, oo,
           static_cast<unsigned>(o), o < kBuilderLen ? "differs" : "same", ti);
}

// Mesh calls compared per check frame (each copies the 18 KB builder state
// a few times; a frame can build hundreds of meshes).
constexpr unsigned kMeshChecksPerFrame = 48;
unsigned g_mesh_checks_left = 0;

int __fastcall MeshTriHook(uint8_t* n, void*, int L, int cnt, int x, int y, int dx, int dy, int* out) {
  if (!g_check_mesh || L != 8 || !g_mesh_checks_left || !--g_mesh_checks_left)
    return tf::MeshTri(reinterpret_cast<tf::Node*>(n), L, cnt, x, y, dx, dy, out);
  static std::vector<uint8_t> pb, gb;
  static std::vector<uint16_t> pi, gi;
  pb.assign(reinterpret_cast<uint8_t*>(kBuilder), reinterpret_cast<uint8_t*>(kBuilder) + kBuilderLen);
  pi.assign(reinterpret_cast<uint16_t*>(kMeshIdx), reinterpret_cast<uint16_t*>(kMeshIdx) + kMeshIdxMax);
  const int out0 = *out;
  g_h_tri.Off();
  const int rg = reinterpret_cast<MeshTriFn>(kMeshTriFn)(n, nullptr, L, cnt, x, y, dx, dy, out);
  g_h_tri.On();
  const int og = *out;
  gb.assign(reinterpret_cast<uint8_t*>(kBuilder), reinterpret_cast<uint8_t*>(kBuilder) + kBuilderLen);
  gi.assign(reinterpret_cast<uint16_t*>(kMeshIdx), reinterpret_cast<uint16_t*>(kMeshIdx) + kMeshIdxMax);
  std::memcpy(reinterpret_cast<void*>(kBuilder), pb.data(), kBuilderLen);
  std::memcpy(reinterpret_cast<void*>(kMeshIdx), pi.data(), kMeshIdxMax * 2);
  *out = out0;
  const int ro = tf::MeshTri(reinterpret_cast<tf::Node*>(n), L, cnt, x, y, dx, dy, out);
  CheckMeshResult("mesh", n, rg, ro, og, *out, cnt, gb, gi);
  return ro;
}

int __fastcall MeshCornerHook(uint8_t* n, void*, int t, int x, int y, int dx, int dy) {
  if (!g_check_mesh || !g_mesh_checks_left) return tf::MeshCorner(t, x, y, dx, dy);
  static std::vector<uint8_t> pb, gb;
  static std::vector<uint16_t> gi;
  pb.assign(reinterpret_cast<uint8_t*>(kBuilder), reinterpret_cast<uint8_t*>(kBuilder) + kBuilderLen);
  uint16_t* ix = reinterpret_cast<uint16_t*>(kMeshIdx);
  uint16_t pi[3] = {ix[t * 3], ix[t * 3 + 1], ix[t * 3 + 2]};
  g_h_corner.Off();
  const int rg = reinterpret_cast<MeshCornerFn>(kMeshCornerFn)(n, nullptr, t, x, y, dx, dy);
  g_h_corner.On();
  gb.assign(reinterpret_cast<uint8_t*>(kBuilder), reinterpret_cast<uint8_t*>(kBuilder) + kBuilderLen);
  gi.assign(ix, ix + kMeshIdxMax);
  std::memcpy(reinterpret_cast<void*>(kBuilder), pb.data(), kBuilderLen);
  std::memcpy(ix + t * 3, pi, sizeof(pi));
  const int ro = tf::MeshCorner(t, x, y, dx, dy);
  CheckMeshResult("mesh corner", n, rg, ro, 0, 0, t, gb, gi);
  return ro;
}

void SetFast(bool on) {
  if (on) {
    g_h_detail.On();
    g_h_draw.On();
    g_h_tri.On();
    g_h_corner.On();
  } else {
    g_h_detail.Off();
    g_h_draw.Off();
    g_h_tri.Off();
    g_h_corner.Off();
  }
  g_fast_on = on;
}

bool KeyPressed(int vk, bool& was);
bool OurWindowInFront();

// TerrainCheck=1 / TerrainLog=1: why node meshes are rebuilt. The mesh walk
// (0x47fed0) calls a builder (0x480790, or 0x480160 when data+0x121 bit 8)
// for each node whose data+0x121 bit 1 is set; the builder keeps the mesh in
// data+0x134/0x138 and registers it with the mesh cache (T+0xc88). Each frame
// 0x50cdd0 evicts meshes not drawn last frame from that cache once they pass
// 1 MB (down to 512 KB), and terrain textures from T+0xc84 the same way; an
// evicted mesh (callback 0x47fc10) is rebuilt when its node is drawn again.
// The builder call sites are wrapped to sort each rebuild by what changed
// since that node's last build.
struct MeshRec {
  uint32_t frame;
  uint32_t vis;      // node+0x40
  uint32_t tex;      // D+8 (the node's terrain texture)
  uint32_t counts;   // data+0x12a | data+0x12c << 16
};
struct MeshWhy {
  unsigned builds = 0, forced = 0;
  unsigned first = 0, evicted = 0, vis = 0, tex = 0, counts = 0, same = 0;
  unsigned age1 = 0, age10 = 0, age_old = 0;
  unsigned evict_calls[2] = {}, evict_n[2] = {};
  uint64_t evict_bytes[2] = {};
  uint32_t cache[2] = {};  // cache objects seen (mesh, texture)
} g_why;
std::vector<std::pair<uint8_t*, MeshRec>> g_mesh_recs;  // open hash on node pointer
uint32_t g_frame_no = 0;
bool g_why_hooked = false;

MeshRec* FindRec(uint8_t* n, bool& fresh) {
  if (g_mesh_recs.empty()) g_mesh_recs.resize(1u << 16);
  const size_t mask = g_mesh_recs.size() - 1;
  size_t i = (reinterpret_cast<uintptr_t>(n) >> 4) * 2654435761u & mask;
  for (size_t probe = 0; probe < 64; ++probe, i = (i + 1) & mask) {
    auto& e = g_mesh_recs[i];
    if (e.first == n) { fresh = false; return &e.second; }
    if (!e.first) { e.first = n; fresh = true; return &e.second; }
  }
  fresh = true;
  return nullptr;  // table crowded: not tracked
}

void NoteBuild(uint8_t* n, bool forced) {
  uint8_t* d = At<uint8_t*>(n, 0x38);
  uint8_t* D = At<uint8_t*>(n, 0x3c);
  MeshRec now{g_frame_no, At<uint32_t>(n, 0x40), D ? At<uint32_t>(D, 8) : 0,
              uint32_t(At<uint16_t>(d, 0x12a)) | uint32_t(At<uint16_t>(d, 0x12c)) << 16};
  ++g_why.builds;
  if (forced) ++g_why.forced;
  bool fresh = true;
  MeshRec* r = FindRec(n, fresh);
  if (!r || fresh) {
    ++g_why.first;
  } else {
    const uint32_t age = g_frame_no - r->frame;
    if (age <= 1) ++g_why.age1; else if (age <= 10) ++g_why.age10; else ++g_why.age_old;
    if (!At<uint32_t>(d, 0x134)) ++g_why.evicted;
    else if (now.vis != r->vis) ++g_why.vis;
    else if (now.tex != r->tex) ++g_why.tex;
    else if (now.counts != r->counts) ++g_why.counts;
    else ++g_why.same;
  }
  if (r) *r = now;
}

int __fastcall BuildWrap(uint8_t* n, void*) {
  NoteBuild(n, false);
  return reinterpret_cast<ThisVoid>(0x480790)(n, nullptr);
}
int __fastcall BuildFullWrap(uint8_t* n, void*) {
  NoteBuild(n, true);
  return reinterpret_cast<ThisVoid>(0x480160)(n, nullptr);
}
using EvictFn = int(__fastcall*)(void* cache, void* edx, int target);
int Evict(int which, uint8_t* cache, int target) {
  const uint32_t n0 = At<uint32_t>(cache, 4), b0 = At<uint32_t>(cache, 0x10);
  const int r = reinterpret_cast<EvictFn>(0x4011B0)(cache, nullptr, target);
  ++g_why.evict_calls[which];
  g_why.evict_n[which] += n0 - At<uint32_t>(cache, 4);
  g_why.evict_bytes[which] += b0 - At<uint32_t>(cache, 0x10);
  g_why.cache[which] = reinterpret_cast<uint32_t>(cache);
  return r;
}
int __fastcall EvictMeshWrap(uint8_t* cache, void*, int target) { return Evict(0, cache, target); }
int __fastcall EvictTexWrap(uint8_t* cache, void*, int target) { return Evict(1, cache, target); }

void InstallMeshWhy() {
  const uint8_t b_full[] = {0xE8, 0xC1, 0x00, 0x00, 0x00};
  const uint8_t b_norm[] = {0xE8, 0xEA, 0x06, 0x00, 0x00};
  const uint8_t b_tex[] = {0xE8, 0x5B, 0x43, 0xEF, 0xFF};
  const uint8_t b_mesh[] = {0xE8, 0x0F, 0x43, 0xEF, 0xFF};
  const uint8_t b_flush[] = {0xE8, 0x05, 0x64, 0xEF, 0xFF};
  if (!patch::Matches(0x48009A, b_full, 5) || !patch::Matches(0x4800A1, b_norm, 5) ||
      !patch::Matches(0x50CE50, b_tex, 5) || !patch::Matches(0x50CE9C, b_mesh, 5) ||
      !patch::Matches(0x50ADA6, b_flush, 5)) {
    patch::Log("terrain: mesh rebuild log: code differs, not applied");
    return;
  }
  patch::Call(0x48009A, reinterpret_cast<const void*>(&BuildFullWrap), 5);
  patch::Call(0x4800A1, reinterpret_cast<const void*>(&BuildWrap), 5);
  patch::Call(0x50CE50, reinterpret_cast<const void*>(&EvictTexWrap), 5);
  patch::Call(0x50CE9C, reinterpret_cast<const void*>(&EvictMeshWrap), 5);
  patch::Call(0x50ADA6, reinterpret_cast<const void*>(&EvictTexWrap), 5);
  g_why_hooked = true;
  patch::Log("terrain: mesh rebuild log on (builders 480790/480160, cache evictions 4011b0 from 50ce50/50ce9c/50ada6)");
}

void MeshWhyReport(unsigned frames) {
  if (!g_why_hooked) return;
  const double f = frames ? frames : 1;
  const MeshWhy& w = g_why;
  auto cache_line = [](uint32_t c, char* out, size_t sz) {
    if (!c) { std::snprintf(out, sz, "not seen"); return; }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(c);
    std::snprintf(out, sz, "%u entries, %.0f KB", At<uint32_t>(p, 4), At<uint32_t>(p, 0x10) / 1024.0);
  };
  char mc[64], tc[64];
  cache_line(w.cache[0], mc, sizeof(mc));
  cache_line(w.cache[1], tc, sizeof(tc));
  patch::Log("terrain mesh rebuilds 5s, per frame: %.1f built (%.1f full); first build %.1f, after eviction %.1f, "
             "visibility changed %.1f, texture changed %.1f, neighbour counts changed %.1f, nothing seen changed "
             "%.1f; last built 1 frame before %.1f, 2-10 frames %.1f, longer %.1f",
             w.builds / f, w.forced / f, w.first / f, w.evicted / f, w.vis / f, w.tex / f, w.counts / f, w.same / f,
             w.age1 / f, w.age10 / f, w.age_old / f);
  patch::Log("terrain caches 5s: mesh cache %s (mesh bytes %u KB, peak %u KB), evicted %.1f meshes / %.0f KB per "
             "frame; texture cache %s, evicted %.1f textures / %.0f KB per frame (%u calls)",
             mc, *reinterpret_cast<uint32_t*>(0x690440) / 1024, *reinterpret_cast<uint32_t*>(0x69043C) / 1024,
             w.evict_n[0] / f, w.evict_bytes[0] / 1024.0 / f, tc, w.evict_n[1] / f, w.evict_bytes[1] / 1024.0 / f,
             w.evict_calls[1]);
  const MeshWhy keep_caches = g_why;
  g_why = MeshWhy();
  g_why.cache[0] = keep_caches.cache[0];
  g_why.cache[1] = keep_caches.cache[1];
}

void FastReport();

void __fastcall EvalHook(uint8_t* root, void*) {
  ++g_frame_no;
  static bool f8 = false;
  if (KeyPressed(VK_F8, f8) && g_fast_hooked && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && OurWindowInFront()) {
    SetFast(!g_fast_on);
    patch::Log("terrain: FastTerrain %s (Ctrl+F8)", g_fast_on ? "ON" : "off");
    dd::DebugNotice(g_fast_on ? "FastTerrain ON" : "FastTerrain off");
  }
  prof::Scope z(prof::kTerrEval);
  // The game thread's x87 control word as the game's C runtime left it
  // (53-bit precision), taken at the first frame. Something that changes it
  // later (a library on the game thread) would change the game's own maths
  // too; it is put back and logged.
  static uint16_t cw0 = 0;
  static bool have_cw0 = false;
  {
    uint16_t cw;
    uint32_t csr;
    FpuGuard::Save(&cw, &csr);
    if (!have_cw0) {
      have_cw0 = true;
      cw0 = cw;
      patch::Log("terrain: game thread FPU control word %04x at the first frame", cw);
    } else if (cw != cw0) {
      static unsigned fixes = 0;
      if (++fixes <= 10)
        patch::Log("terrain: game thread FPU control word changed from %04x to %04x; put back%s", cw0, cw,
                   fixes == 10 ? " (no more of these lines)" : "");
      FpuGuard::Restore(cw0, csr);
    }
  }
  if (!g_fast_on || !tf::FpuIs53()) {
    if (g_fast_on) {
      patch::Log("terrain: FastTerrain: the FPU is not at 53-bit precision; switched off");
      SetFast(false);
    }
    g_h_eval.Off();
    reinterpret_cast<EvalFn>(kEvalFn)(root, nullptr);
    g_h_eval.On();
    return;
  }
  SyncFastOptions();
  g_check_detail = false;  // Detail was compared during this frame's Setup
  const Config& c = GetConfig();
  bool check = false;
  if (c.terrain_check) {
    const DWORD now = GetTickCount();
    if (now - g_check_last >= 2000) {
      g_check_last = now;
      check = true;
    }
  }
  g_check_mesh = false;
  if (check) {
    CheckEval(root);
    g_check_mesh = true;    // this frame's mesh building (after Eval)
    g_mesh_checks_left = kMeshChecksPerFrame;
    g_check_draw = true;    // this frame's draw walk
    g_check_detail = true;  // next frame's Setup
  } else {
    tf::Eval(reinterpret_cast<tf::Node*>(root));
  }
  FastReport();
}

void FastReport() {
  static DWORD last = 0;
  static unsigned frames = 0;
  ++frames;
  const DWORD now = GetTickCount();
  if (!last) last = now;
  if (now - last < 5000) return;
  const Config& c = GetConfig();
  if (c.terrain_check || c.terrain_log) {
    const double f = frames ? frames : 1;
    patch::Log("terrain fast 5s: %u frames; per frame: nodes evaluated %.0f, vertices tested %.0f, sub-blocks walked "
               "%.0f, draw calls %.0f (%.0f far-coloured, %.0f vertices), meshes built %.1f (%.0f vertices); detail "
               "numbers clamped %u",
               frames, tf::g_stats.eval_nodes / f, tf::g_stats.eval_tests / f, tf::g_stats.draw_blocks / f,
               tf::g_stats.draw_calls / f, tf::g_stats.draw_far / f, tf::g_stats.draw_verts / f,
               tf::g_stats.mesh_calls / 2.0 / f, tf::g_stats.mesh_verts / f, tf::g_stats.lod_over);
  }
  if (c.terrain_check)
    patch::Log("terrain check 5s: Detail %u calls, %u differ; Eval %u frames (%u nodes), %u nodes and %u terrains differ "
               "(%.1f ms per check); draw walk %u walks (%u calls), %u differ; mesh %u calls, %u differ",
               g_chk.detail, g_chk.detail_bad, g_chk.eval, g_chk.eval_nodes, g_chk.eval_bad_nodes,
               g_chk.eval_bad_terrain, g_chk.eval ? g_chk.eval_ms / g_chk.eval : 0.0, g_chk.draw, g_chk.draw_calls,
               g_chk.draw_bad, g_chk.mesh, g_chk.mesh_bad);
  MeshWhyReport(frames);
  const unsigned logged = g_chk.logged;
  g_chk = CheckStats();
  g_chk.logged = logged;
  g_hole_parents += tf::g_stats.hole_parents;
  g_hole_fixed += tf::g_stats.hole_fixed;
  g_lod_calls += tf::g_stats.lod_calls;
  g_lod_over += tf::g_stats.lod_over;
  tf::g_stats = tf::Stats();
  frames = 0;
  last = now;
}

void InstallFastTerrain() {
  if (!GetConfig().fast_terrain) {
    patch::Log("terrain: FastTerrain=0, the game's terrain code runs as it is");
    return;
  }
  if (!simd::CpuHasSse2()) {
    patch::Log("terrain: FastTerrain needs SSE2; the game's terrain code runs as it is");
    return;
  }
  static const uint8_t detail0[] = {0x83, 0xEC, 0x1C, 0x56, 0x8B, 0xF1};  // sub esp,1ch / push esi / mov esi,ecx
  static const uint8_t eval0[] = {0x83, 0xEC, 0x0C, 0x56, 0x8B, 0xF1};    // sub esp,0ch / push esi / mov esi,ecx
  static const uint8_t draw0[] = {0x53, 0x55, 0x56, 0x8B, 0xF1, 0x57};    // push ebx/ebp/esi / mov esi,ecx / push edi
  static const uint8_t tri0[] = {0x53, 0x55, 0x8B, 0x6C, 0x24, 0x1C};     // push ebx/ebp / mov ebp,[esp+1ch]
  static const uint8_t corner0[] = {0x8B, 0x44, 0x24, 0x14, 0x8B, 0x4C};  // mov eax,[esp+14h] / mov ecx,...
  if (!patch::Matches(kDetailFn, detail0, 6) || !patch::Matches(kEvalFn, eval0, 6) ||
      !patch::Matches(kDrawWalkFn, draw0, 6) || !patch::Matches(kMeshTriFn, tri0, 6) ||
      !patch::Matches(kMeshCornerFn, corner0, 6)) {
    patch::Log("terrain: FastTerrain: code differs, not applied");
    return;
  }
  g_detail_tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!g_detail_tramp) return;
  std::memcpy(g_detail_tramp, detail0, 6);
  g_detail_tramp[6] = 0xE9;
  const int32_t rel = static_cast<int32_t>((kDetailFn + 6) - (reinterpret_cast<uintptr_t>(g_detail_tramp) + 11));
  std::memcpy(g_detail_tramp + 7, &rel, 4);
  g_h_detail.at = kDetailFn;
  g_h_eval.at = kEvalFn;
  g_h_draw.at = kDrawWalkFn;
  g_h_detail.Install(reinterpret_cast<const void*>(&DetailHook));
  g_h_eval.Install(reinterpret_cast<const void*>(&EvalHook));
  g_h_draw.Install(reinterpret_cast<const void*>(&DrawHook));
  g_h_tri.at = kMeshTriFn;
  g_h_corner.at = kMeshCornerFn;
  g_h_tri.Install(reinterpret_cast<const void*>(&MeshTriHook));
  g_h_corner.Install(reinterpret_cast<const void*>(&MeshCornerHook));
  g_fast_hooked = g_fast_on = true;
  SyncFastOptions();
  patch::Log("terrain: FastTerrain on (detail numbers 482550, Eval 4836e0, draw walk 4818b0, mesh 480c30/481640 rewritten; Ctrl+F8 "
             "switches it)%s",
             GetConfig().terrain_check ? "; TerrainCheck=1: compared with the game's code every 2 s" : "");
}

// Profile=1: time the terrain phases the rewrites leave to the game.
using SetupFn = int(__fastcall*)(void* root, void* edx, int a, int b);
// GpuTerrain (gputerrain.cpp): while it draws the terrain, the game's own
// terrain phases (node setup, Eval, mesh building, drawing) are skipped.
int __fastcall SetupZone(void* root, void*, int a, int b) {
  if (GpuTerrainActive()) return 1;
  prof::Scope z(prof::kTerrSetup);
  return reinterpret_cast<SetupFn>(0x482D20)(root, nullptr, a, b);
}
int __fastcall EvalSite(void* root, void*) {
  if (GpuTerrainActive()) return 0;
  return reinterpret_cast<ThisVoid>(kEvalFn)(root, nullptr);
}
int __fastcall MeshZone(void* root, void*) {
  if (GpuTerrainActive()) return 1;
  prof::Scope z(prof::kTerrMesh);
  return reinterpret_cast<ThisVoid>(0x47FEC0)(root, nullptr);
}
int __fastcall BlocksZone(void* root, void*) {
  if (GpuTerrainPrepare(root)) return 1;
  prof::Scope z(prof::kTerrBlocks);
  return reinterpret_cast<ThisVoid>(kRender)(root, nullptr);
}

// Where the game draws its terrain (0x50bfaf / 0x50bffc: call 481870,
// thiscall root, arg the device wrapper, ret 4), inside the scene with the
// camera of this frame.
using TerrainDrawFn = void(__fastcall*)(void* root, void* edx, void* dev);
// GpuTerrainLog: log the game's terrain pass state at each site once at the
// start and again each time Ctrl+F7 changes GpuTerrain (the draws are logged
// only while the game draws them itself).
bool g_probe_due[3] = {true, true, true};
bool g_probe_was_on = false;
bool ProbeDue(int site) {
  const bool on = GpuTerrainActive();
  if (on != g_probe_was_on) {
    g_probe_was_on = on;
    g_probe_due[0] = g_probe_due[1] = g_probe_due[2] = true;
  }
  if (!GetConfig().gpu_terrain || !GetConfig().gpu_terrain_log || !g_probe_due[site]) return false;
  g_probe_due[site] = false;
  return true;
}

void __fastcall TerrainDrawSite(void* root, void*, void* dev) {
  GpuTerrainKeys();
  if (GetConfig().gpu_terrain) dd::GpuTerrainCaptureState(0, ProbeDue(0));
  if (GpuTerrainDraw(root)) return;
  // RiderShadows: the game's shadow pre-draw no longer runs on its own
  // (gterr-15 log: 0 game pre-draws with either terrain), so the frame's
  // shadows are drawn before the game's terrain patches too.
  dd::ShadowEnsureFrame();
  reinterpret_cast<TerrainDrawFn>(0x481870)(root, nullptr, dev);
  // GpuTerrainLog: every 2 s while the game draws its own terrain, where it
  // draws it near and far (to match GpuTerrain to it).
  static DWORD last = 0;
  float eye[3];
  if (GetConfig().gpu_terrain && GetConfig().gpu_terrain_log && GetTickCount() - last > 2000 && FreeCamEye(eye)) {
    last = GetTickCount();
    GpuTerrainProbeGame(root, eye);
  }
}

// 0x481890 (thiscall root; device wrapper, texture; ret 8) walks the game's
// terrain once more: at 0x50bf2b it is the one draw of the terrain (detail on
// stage 1), at 0x50c0bf the detail texture's second pass. GpuTerrain draws
// both itself, so the game's (stale) blocks must not be drawn over it.
using TerrainPassFn = int(__fastcall*)(void* root, void* edx, void* dev, void* tex);
int __fastcall TerrainOnePassSite(void* root, void*, void* dev, void* tex) {
  GpuTerrainKeys();
  if (GetConfig().gpu_terrain) dd::GpuTerrainCaptureState(1, ProbeDue(1));
  if (GpuTerrainDraw(root)) return 1;
  dd::ShadowEnsureFrame();
  return reinterpret_cast<TerrainPassFn>(0x481890)(root, nullptr, dev, tex);
}
int __fastcall TerrainDetailPassSite(void* root, void*, void* dev, void* tex) {
  if (GetConfig().gpu_terrain) dd::GpuTerrainCaptureState(2, ProbeDue(2));
  if (GpuTerrainActive()) return 1;
  return reinterpret_cast<TerrainPassFn>(0x481890)(root, nullptr, dev, tex);
}

}  // namespace

void ApplyTerrainFixes() {
  const Config& c = GetConfig();
  if (c.terrain_lod_fix || c.terrain_log) {
    const uint8_t call_ftol[] = {0xE8, 0xE4, 0x78, 0x0B, 0x00};  // call 53a0f8
    const uint8_t store[] = {0x66, 0x89, 0x87, 0x22, 0x01, 0x00, 0x00};  // mov [edi+122h],ax
    if (patch::Matches(kLodCall, call_ftol, 5) && patch::Matches(kLodCall + 5, store, sizeof(store))) {
      g_lod_fix = c.terrain_lod_fix ? 1 : 0;
      patch::Call(kLodCall, reinterpret_cast<const void*>(&TerrainLodFtol), 5);
      g_lod_hooked = true;
      patch::Log("terrain: TerrainLodFix %s (far-block detail number clamped at 65535)", g_lod_fix ? "on" : "off (counting only)");
    } else {
      patch::Log("terrain: TerrainLodFix: code differs, not applied");
    }
  }
  {
    const uint8_t cmp_jne[] = {0x39, 0x6E, 0x40, 0x75, 0x03};  // cmp [esi+40h],ebp; jne 483725
    g_hole_fix = c.terrain_hole_fix ? 1 : 0;
    if (c.terrain_hole_fix || c.terrain_log) {
      if (patch::Matches(kHoleCmp, cmp_jne, 5)) {
        patch::Jump(kHoleCmp, reinterpret_cast<uintptr_t>(&TerrainVisStub), 5);
        g_hole_hooked = true;
        patch::Log("terrain: TerrainHoleFix %s", g_hole_fix ? "on" : "off (counting only)");
      } else {
        patch::Log("terrain: TerrainHoleFix: code differs, not applied");
      }
    }
  }
  g_detail = g_detail_ini = c.terrain_detail;
  if (c.terrain_detail != 1.0f || c.terrain_log) {
    const uint8_t call_params[] = {0xE8, 0x58, 0x03, 0x00, 0x00};  // call 50d350
    if (patch::Matches(kParamsCall, call_params, 5)) {
      patch::Call(kParamsCall, reinterpret_cast<const void*>(&ParamsHook), 5);
      g_params_hooked = true;
      patch::Log("terrain: TerrainDetail x%.2f", g_detail);
    } else {
      patch::Log("terrain: TerrainDetail: code differs, not applied");
    }
  }
  InstallFastTerrain();
  if (c.terrain_check || c.terrain_log) InstallMeshWhy();
  // TerrainMeshCache: the game keeps at most 1 MB of terrain meshes that were
  // not drawn last frame (then trims to 512 KB, 0x50ce86/0x50ce96); at long
  // view distances it throws out and rebuilds hundreds of meshes a frame.
  if (c.terrain_mesh_cache_kb > 0 && c.terrain_mesh_cache_kb != 1024) {
    const uint8_t cmp_limit[] = {0x81, 0xF9, 0x00, 0x00, 0x10, 0x00};  // cmp ecx,100000h
    const uint8_t add_keep[] = {0x05, 0x00, 0x00, 0x08, 0x00};         // add eax,80000h
    if (patch::Matches(0x50CE86, cmp_limit, 6) && patch::Matches(0x50CE96, add_keep, 5)) {
      const int kb = c.terrain_mesh_cache_kb < 256 ? 256 : (c.terrain_mesh_cache_kb > 1048576 ? 1048576 : c.terrain_mesh_cache_kb);
      const uint32_t limit = static_cast<uint32_t>(kb) * 1024u, keep = limit / 2;
      patch::Write(0x50CE88, &limit, 4);
      patch::Write(0x50CE97, &keep, 4);
      patch::Log("terrain: TerrainMeshCache %d KB (meshes not drawn last frame kept up to that, trimmed to half; game "
                 "1024 KB)", kb);
    } else {
      patch::Log("terrain: TerrainMeshCache: code differs, not applied");
    }
  }
  if (c.gpu_terrain) {
    // The terrain phases' call sites (shared with Profile=1's zones).
    const uint8_t call_setup[] = {0xE8, 0x0F, 0x5D, 0xF7, 0xFF};   // call 482d20
    const uint8_t call_eval[] = {0xE8, 0x9D, 0x66, 0xF7, 0xFF};    // call 4836e0
    const uint8_t call_mesh[] = {0xE8, 0x44, 0x2E, 0xF7, 0xFF};    // call 47fec0
    const uint8_t call_blocks[] = {0xE8, 0x4B, 0x50, 0xF7, 0xFF};  // call 4820e0
    const uint8_t call_draw1[] = {0xE8, 0xBC, 0x58, 0xF7, 0xFF};   // 50bfaf: call 481870
    const uint8_t call_draw2[] = {0xE8, 0x6F, 0x58, 0xF7, 0xFF};   // 50bffc: call 481870
    if (patch::Matches(0x50D00C, call_setup, 5) && patch::Matches(0x50D03E, call_eval, 5) &&
        patch::Matches(0x50D077, call_mesh, 5) && patch::Matches(0x50D090, call_blocks, 5) &&
        patch::Matches(0x50BFAF, call_draw1, 5) && patch::Matches(0x50BFFC, call_draw2, 5)) {
      patch::Call(0x50BFAF, reinterpret_cast<const void*>(&TerrainDrawSite), 5);
      patch::Call(0x50BFFC, reinterpret_cast<const void*>(&TerrainDrawSite), 5);
      const uint8_t call_pass1[] = {0xE8, 0x60, 0x59, 0xF7, 0xFF};  // 50bf2b: call 481890
      const uint8_t call_pass2[] = {0xE8, 0xCC, 0x57, 0xF7, 0xFF};  // 50c0bf: call 481890
      if (patch::Matches(0x50BF2B, call_pass1, 5) && patch::Matches(0x50C0BF, call_pass2, 5)) {
        patch::Call(0x50BF2B, reinterpret_cast<const void*>(&TerrainOnePassSite), 5);
        patch::Call(0x50C0BF, reinterpret_cast<const void*>(&TerrainDetailPassSite), 5);
      } else {
        patch::Log("terrain: GpuTerrain: detail pass code differs; the game's detail pass is left as it is");
      }
      patch::Call(0x50D00C, reinterpret_cast<const void*>(&SetupZone), 5);
      patch::Call(0x50D03E, reinterpret_cast<const void*>(&EvalSite), 5);
      patch::Call(0x50D077, reinterpret_cast<const void*>(&MeshZone), 5);
      if (!c.terrain_log) patch::Call(0x50D090, reinterpret_cast<const void*>(&BlocksZone), 5);
      patch::Log("terrain: GpuTerrain on (Ctrl+F7 switches between it and the game's terrain)");
    } else {
      patch::Log("terrain: GpuTerrain: code differs, not applied");
    }
  }
  if (c.profile) {
    const uint8_t call_setup[] = {0xE8, 0x0F, 0x5D, 0xF7, 0xFF};   // call 482d20
    const uint8_t call_mesh[] = {0xE8, 0x44, 0x2E, 0xF7, 0xFF};    // call 47fec0
    const uint8_t call_blocks[] = {0xE8, 0x4B, 0x50, 0xF7, 0xFF};  // call 4820e0
    if (patch::Matches(0x50D00C, call_setup, 5)) patch::Call(0x50D00C, reinterpret_cast<const void*>(&SetupZone), 5);
    if (patch::Matches(0x50D077, call_mesh, 5)) patch::Call(0x50D077, reinterpret_cast<const void*>(&MeshZone), 5);
    if (!c.terrain_log && patch::Matches(0x50D090, call_blocks, 5))
      patch::Call(0x50D090, reinterpret_cast<const void*>(&BlocksZone), 5);
    // Without FastTerrain, Eval and the draw walk get their zones here.
    if (!g_fast_hooked) {
      static const uint8_t eval0[] = {0x83, 0xEC, 0x0C, 0x56, 0x8B, 0xF1};
      if (patch::Matches(kEvalFn, eval0, 6)) {
        g_h_eval.at = kEvalFn;
        g_h_eval.Install(reinterpret_cast<const void*>(&EvalHook));
      }
    }
    patch::Log("terrain: Profile=1, terrain phases timed (zones tsetup, teval, tmesh, tblocks, tdraw)");
  }
  if (!c.terrain_log) return;
  const uint8_t call_render[] = {0xE8, 0x4B, 0x50, 0xF7, 0xFF};  // call 4820e0
  if (!patch::Matches(kRenderCall, call_render, 5)) {
    patch::Log("terrain log: code differs, not applied");
    return;
  }
  patch::Call(kRenderCall, reinterpret_cast<const void*>(&RenderHook), 5);
  const uint8_t call_leaf[] = {0xE8, 0x5A, 0x19, 0x0B, 0x00};  // call 534be0
  if (patch::Matches(kLeafCullCall, call_leaf, 5))
    patch::Call(kLeafCullCall, reinterpret_cast<const void*>(&LeafCull), 5);
  else
    patch::Log("terrain log: leaf cull call differs, Ctrl+F10 does nothing");
  if (HANDLE t = CreateThread(nullptr, 0, KeyThread, nullptr, 0, nullptr)) CloseHandle(t);
  patch::Log("terrain log: on (Ctrl+F9 TerrainHoleFix on/off, Ctrl+F10 TerrainDetail x1/x2/x4/x0.5/x0.25, Ctrl+F11 dump + probe)");
}
