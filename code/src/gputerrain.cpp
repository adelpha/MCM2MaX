// GpuTerrain: see gputerrain.h.
//
// The game's terrain (the object passed around as T; root node pointer at
// T+0x44) is a tree of nodes, each a 17x17 grid of height points over 16x16
// cells; a cell either has a child node (16x16 finer cells) or is drawn at
// this node's resolution. Height grids are shared tiles, all loaded with the
// track (T+0xc14[count T+0xc10], 0xd50 bytes each: 289 x 8-byte points,
// height word at +2, base at +0xd4c). Nodes are loaded on demand (0x483440):
// a node's children are listed at T+0xc38 + node+0x2c in the track file (256
// offsets), each a record (0x47f550): s10, s14, y min, y max (float), level
// word, tile number (word), texture code, 16 sub-block texture codes, a
// length-prefixed blob, a byte, and its own children's list offset. Children
// not loaded yet are read here with the game's stream functions (seek
// 0x4622b0, read 0x4625b0), without creating game nodes.
//
// Point (x, y) of a node is at x = ((gx + x) << shift) * scale, z likewise,
// height ((h - base) * s10 + base) * s14 * scale (scale = T+0x40) -- the
// mesh builder's (0x47ede0) and the collision's (0x48559d) formula.
#include "gputerrain.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "dd/racetitle.h"
#include "patch.h"

namespace {

template <class T>
T Rd(const void* p, int off) {
  T v;
  std::memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof(T));
  return v;
}

constexpr int kChunkVerts = 32768;  // vertices per chunk (16-bit indices from its base)
constexpr int kNodeVerts = 289 + 4 * 17;

using SeekFn = int(__fastcall*)(void* stream, void* edx, int pos, int a, int b);
using ReadFn = int(__fastcall*)(void* stream, void* edx, void* dst, int size, int count);
const SeekFn kSeek = reinterpret_cast<SeekFn>(0x4622B0);
const ReadFn kRead = reinterpret_cast<ReadFn>(0x4625B0);

struct NodeInfo {
  int gx, gy, shift, level;
  float s10, s14;
  const uint8_t* tile;
  uint32_t texcode;
};

struct BuildStats {
  unsigned nodes_mem = 0, nodes_file = 0, nodes_drawn = 0, leaf_cells = 0, skirt_quads = 0;
  unsigned bad_tile = 0, bad_record = 0, kid_tables = 0;
  unsigned by_level[16] = {};
  double read_ms = 0;
};

double Now() {
  static LARGE_INTEGER f = {};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

struct Builder {
  uint8_t* T = nullptr;
  void* stream = nullptr;
  uint32_t file_base = 0;
  uint32_t ntiles = 0;
  uint8_t* const* tiles = nullptr;
  double scale = 1;
  int log = 0;
  gt::Mesh* m = nullptr;
  BuildStats st;
  gt::Chunk cur{};
  bool open = false;
  std::vector<uint8_t> blob;

  void CloseChunk() {
    if (open && cur.index_count) m->chunks.push_back(cur);
    open = false;
  }
  void Room(unsigned verts) {
    if (open && cur.vertex_count + verts > kChunkVerts) CloseChunk();
    if (!open) {
      cur = gt::Chunk{};
      cur.first_index = static_cast<uint32_t>(m->idx.size());
      cur.base_vertex = static_cast<int32_t>(m->verts.size());
      cur.lo[0] = cur.lo[1] = cur.lo[2] = 3.0e38f;
      cur.hi[0] = cur.hi[1] = cur.hi[2] = -3.0e38f;
      open = true;
    }
  }

  bool Seek(uint32_t off) { return kSeek(stream, nullptr, static_cast<int>(file_base + off), 0, 1) == 0; }
  bool Read(void* dst, int size, int count = 1) { return kRead(stream, nullptr, dst, size, count) == count; }

  // A child record (0x47f550's reads). Returns false if it doesn't make sense.
  bool ReadRecord(uint32_t off, NodeInfo& ni, uint32_t& kids_off) {
    if (!Seek(off)) return false;
    float f[4];
    uint16_t w[2];
    uint32_t tex, sub[16], len;
    uint8_t b;
    if (!Read(f, 4, 4) || !Read(w, 2, 2) || !Read(&tex, 4) || !Read(sub, 4, 16) || !Read(&len, 4)) return false;
    if (len > (16u << 20)) return false;
    if (len) {
      blob.resize(len);
      if (!Read(blob.data(), static_cast<int>(len))) return false;
    }
    if (!Read(&b, 1) || !Read(&kids_off, 4)) return false;
    if (!std::isfinite(f[0]) || !std::isfinite(f[1])) return false;
    ni.s10 = f[0];
    ni.s14 = f[1];
    ni.texcode = tex;
    if (w[1] >= ntiles || !tiles[w[1]]) {
      ++st.bad_tile;
      ni.tile = nullptr;
    } else {
      ni.tile = tiles[w[1]];
    }
    return true;
  }

  float Height(const NodeInfo& ni, int idx) const {
    const int32_t base = Rd<int32_t>(ni.tile, 0xD4C);
    const uint32_t h = Rd<uint16_t>(ni.tile, idx * 8 + 2);
    double y = static_cast<double>(static_cast<int32_t>(h - static_cast<uint32_t>(base))) * ni.s10;
    y = (y + static_cast<double>(base)) * ni.s14;
    return static_cast<float>(y * scale);
  }
  float Coord(int g, int shift) const {
    return static_cast<float>(static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(g) << (shift & 31))) * scale);
  }

  static uint32_t Shade(float nx, float ny, float nz, int level, int log) {
    // Untextured first version: sand colour lit by a fixed sun.
    const float lx = 0.42f, ly = 0.80f, lz = 0.43f;
    float d = nx * lx + ny * ly + nz * lz;
    if (d < 0) d = 0;
    const float k = 0.38f + 0.62f * d;
    float r = 168, g = 138, bl = 104;
    if (log >= 2) {  // GpuTerrainLog=2: tint by node level (where the detail is)
      static const float tint[6][3] = {{1.2f, 0.7f, 0.7f}, {0.7f, 1.2f, 0.7f}, {0.7f, 0.8f, 1.3f},
                                       {1.2f, 1.2f, 0.6f}, {1.2f, 0.7f, 1.2f}, {0.7f, 1.2f, 1.2f}};
      const float* t = tint[(level < 0 ? 0 : level) % 6];
      r *= t[0], g *= t[1], bl *= t[2];
    }
    auto c = [&](float v) {
      v *= k;
      return static_cast<uint32_t>(v < 0 ? 0 : v > 255 ? 255 : v);
    };
    return 0xFF000000u | c(r) << 16 | c(g) << 8 | c(bl);
  }

  void Emit(const NodeInfo& ni, const bool* has_kid) {
    unsigned leaves = 0;
    for (int i = 0; i < 256; ++i) leaves += has_kid[i] ? 0 : 1;
    if (!leaves || !ni.tile) return;
    ++st.nodes_drawn;
    st.leaf_cells += leaves;
    if (ni.level >= 0 && ni.level < 16) ++st.by_level[ni.level];
    Room(kNodeVerts);
    const uint32_t base = cur.vertex_count;
    float H[17][17], X[17], Z[17];
    for (int i = 0; i < 17; ++i) {
      X[i] = Coord(ni.gx + i, ni.shift);
      Z[i] = Coord(ni.gy + i, ni.shift);
    }
    for (int y = 0; y < 17; ++y)
      for (int x = 0; x < 17; ++x) H[y][x] = Height(ni, y * 17 + x);
    const float cell = X[1] - X[0];
    const float inv = cell != 0 ? 1.0f / cell : 0.0f;
    float ymin = 3e38f, ymax = -3e38f;
    for (int y = 0; y < 17; ++y) {
      for (int x = 0; x < 17; ++x) {
        const int x0 = x > 0 ? x - 1 : x, x1 = x < 16 ? x + 1 : x;
        const int y0 = y > 0 ? y - 1 : y, y1 = y < 16 ? y + 1 : y;
        const float dx = (H[y][x1] - H[y][x0]) * inv / static_cast<float>(x1 - x0);
        const float dz = (H[y1][x] - H[y0][x]) * inv / static_cast<float>(y1 - y0);
        float nx = -dx, ny = 1, nz = -dz;
        const float l = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
        gt::Vertex v{};
        v.pos[0] = X[x];
        v.pos[1] = H[y][x];
        v.pos[2] = Z[y];
        v.pos[3] = 1;
        v.diffuse = Shade(nx * l, ny * l, nz * l, ni.level, log);
        m->verts.push_back(v);
        if (H[y][x] < ymin) ymin = H[y][x];
        if (H[y][x] > ymax) ymax = H[y][x];
      }
    }
    auto add = [&](uint32_t a, uint32_t b, uint32_t c) {
      m->idx.push_back(static_cast<uint16_t>(a));
      m->idx.push_back(static_cast<uint16_t>(b));
      m->idx.push_back(static_cast<uint16_t>(c));
      cur.index_count += 3;
    };
    for (int cy = 0; cy < 16; ++cy) {
      for (int cx = 0; cx < 16; ++cx) {
        if (has_kid[cy * 16 + cx]) continue;
        const uint32_t a = base + cy * 17 + cx;
        add(a, a + 17, a + 18);
        add(a, a + 18, a + 1);
      }
    }
    // Skirts: a strip hanging down from each outer edge where a cell is drawn
    // here, so seams with finer or coarser neighbours show no sky.
    const float drop = std::fabs(cell) * 1.5f + 1.0f;
    uint32_t vbase = base + 289;
    const int edges[4][4] = {{0, 0, 1, 0}, {0, 16, 1, 0}, {0, 0, 0, 1}, {16, 0, 0, 1}};  // start x,y, step
    for (int e = 0; e < 4; ++e) {
      for (int k = 0; k < 17; ++k) {
        const int x = edges[e][0] + edges[e][2] * k, y = edges[e][1] + edges[e][3] * k;
        gt::Vertex v = m->verts[cur.base_vertex + base + y * 17 + x];
        v.pos[1] -= drop;
        m->verts.push_back(v);
      }
      for (int k = 0; k < 16; ++k) {
        const int x = edges[e][0] + edges[e][2] * k, y = edges[e][1] + edges[e][3] * k;
        // the cell on the inside of this edge segment
        const int cx = e == 3 ? 15 : (edges[e][2] ? k : 0), cy = e == 1 ? 15 : (edges[e][3] ? k : 0);
        if (has_kid[cy * 16 + cx]) continue;
        const uint32_t t0 = base + y * 17 + x;
        const uint32_t t1 = base + (y + edges[e][3]) * 17 + (x + edges[e][2]);
        const uint32_t b0 = vbase + k, b1 = vbase + k + 1;
        add(t0, b0, b1);
        add(t0, b1, t1);
        ++st.skirt_quads;
      }
      vbase += 17;
    }
    cur.vertex_count += kNodeVerts;
    const float lo[3] = {std::fmin(X[0], X[16]), ymin - drop, std::fmin(Z[0], Z[16])};
    const float hi[3] = {std::fmax(X[0], X[16]), ymax, std::fmax(Z[0], Z[16])};
    for (int i = 0; i < 3; ++i) {
      if (lo[i] < cur.lo[i]) cur.lo[i] = lo[i];
      if (hi[i] > cur.hi[i]) cur.hi[i] = hi[i];
    }
  }

  // A node: its children first (from memory, or from the file), then its own
  // cells that have no child.
  void Visit(const NodeInfo& ni, uint8_t* const* mem_kids, uint32_t kids_off, int depth) {
    bool has_kid[256] = {};
    if (depth < 12) {
      if (mem_kids) {
        for (int i = 0; i < 256; ++i) {
          uint8_t* c = mem_kids[i];
          if (!c) continue;
          has_kid[i] = true;
          ++st.nodes_mem;
          VisitMem(c, depth + 1);
        }
      } else if (kids_off) {
        uint32_t offs[256];
        const double t0 = Now();
        const bool ok = Seek(kids_off) && Read(offs, 4, 256);
        st.read_ms += Now() - t0;
        ++st.kid_tables;
        if (ok) {
          for (int i = 0; i < 256; ++i) {
            if (!offs[i]) continue;
            NodeInfo c{};
            uint32_t ckids = 0;
            const double t1 = Now();
            const bool rec = ReadRecord(offs[i], c, ckids);
            st.read_ms += Now() - t1;
            if (!rec) {
              ++st.bad_record;
              continue;
            }
            // 0x4835a0: the child's place and size
            c.gx = (ni.gx + (i & 15)) * 16;
            c.gy = (ni.gy + (i >> 4)) * 16;
            c.shift = ni.shift - 4;
            c.level = ni.level - 1;
            has_kid[i] = true;
            ++st.nodes_file;
            Visit(c, nullptr, ckids, depth + 1);
          }
        } else {
          ++st.bad_record;
        }
      }
    }
    Emit(ni, has_kid);
  }

  void VisitMem(uint8_t* n, int depth) {
    NodeInfo ni{};
    ni.gx = Rd<uint16_t>(n, 0x24);
    ni.gy = Rd<uint16_t>(n, 0x26);
    ni.level = Rd<uint8_t>(n, 0x28);
    ni.shift = Rd<uint8_t>(n, 0x29);
    ni.s10 = Rd<float>(n, 0x10);
    ni.s14 = Rd<float>(n, 0x14);
    ni.tile = Rd<const uint8_t*>(n, 0x0C);
    const uint8_t* tex = Rd<const uint8_t*>(n, 0x3C);
    ni.texcode = tex ? Rd<uint32_t>(tex, 0x0C) : 0;
    Visit(ni, Rd<uint8_t* const*>(n, 0x04), Rd<uint32_t>(n, 0x2C), depth);
  }
};

// ---- state

gt::Mesh g_mesh;
uint8_t* g_built_T = nullptr;
uint8_t* g_built_root = nullptr;
bool g_failed = false;
bool g_on = true;  // Ctrl+F7

struct DrawLog {
  unsigned frames = 0, chunks = 0, tris = 0, game_frames = 0;
  double ms = 0, max_ms = 0;
  DWORD last = 0;
} g_dl;

bool Build(uint8_t* root) {
  const double t0 = Now();
  Builder b;
  b.T = Rd<uint8_t*>(root, 0x34);
  b.stream = Rd<void*>(b.T, 0x3C);
  b.file_base = Rd<uint32_t>(b.T, 0xC38);
  b.ntiles = Rd<uint32_t>(b.T, 0xC10);
  b.tiles = Rd<uint8_t* const*>(b.T, 0xC14);
  b.scale = Rd<float>(b.T, 0x40);
  b.log = GetConfig().gpu_terrain_log;
  if (!b.stream || !b.tiles || !b.ntiles || b.ntiles > 1000000) {
    patch::Log("gpu terrain: terrain data not as expected (stream %p, %u tiles at %p); the game draws the terrain",
               b.stream, b.ntiles, static_cast<const void*>(b.tiles));
    return false;
  }
  gt::Mesh m;
  m.verts.reserve(1u << 20);
  m.idx.reserve(3u << 20);
  b.m = &m;
  b.VisitMem(root, 0);
  b.CloseChunk();
  const double ms = Now() - t0;
  if (m.chunks.empty()) {
    patch::Log("gpu terrain: no terrain found under the root node (%u nodes); the game draws the terrain",
               b.st.nodes_mem);
    return false;
  }
  m.generation = g_mesh.generation + 1;
  g_mesh = std::move(m);
  const BuildStats& s = b.st;
  const double mb = (g_mesh.verts.size() * sizeof(gt::Vertex) + g_mesh.idx.size() * 2) / 1048576.0;
  patch::Log("gpu terrain: built in %.0f ms (track file reads %.0f ms): %u nodes in memory + %u read from the file "
             "(%u child lists), %u with cells drawn, %u cells, %u skirt quads, %zu vertices, %zu triangles, %zu "
             "chunks, %.1f MB; %u height tiles; %u bad tile numbers, %u unreadable records",
             ms, s.read_ms, s.nodes_mem + 1, s.nodes_file, s.kid_tables, s.nodes_drawn, s.leaf_cells, s.skirt_quads,
             g_mesh.verts.size(), g_mesh.idx.size() / 3, g_mesh.chunks.size(), mb, b.ntiles, s.bad_tile,
             s.bad_record);
  char lv[256];
  int n = 0;
  for (int i = 15; i >= 0; --i)
    if (s.by_level[i]) n += std::snprintf(lv + n, sizeof(lv) - n, " L%d:%u", i, s.by_level[i]);
  lv[n] = 0;
  patch::Log("gpu terrain: nodes with cells drawn by level:%s; root level %u shift %u at %u,%u, scale %.4f", lv,
             Rd<uint8_t>(root, 0x28), Rd<uint8_t>(root, 0x29), Rd<uint16_t>(root, 0x24), Rd<uint16_t>(root, 0x26),
             b.scale);
  if (b.log >= 1) {
    for (size_t i = 0; i < g_mesh.chunks.size() && i < 8; ++i) {
      const gt::Chunk& c = g_mesh.chunks[i];
      patch::Log("gpu terrain: chunk %zu: %u vertices, %u triangles, box %.0f,%.0f,%.0f - %.0f,%.0f,%.0f", i,
                 c.vertex_count, c.index_count / 3, c.lo[0], c.lo[1], c.lo[2], c.hi[0], c.hi[1], c.hi[2]);
    }
  }
  return true;
}

bool KeyHit(int vk, bool& was) {
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool hit = down && !was;
  was = down;
  return hit;
}

void Report(bool drew) {
  if (drew) ++g_dl.frames; else ++g_dl.game_frames;
  const DWORD now = GetTickCount();
  if (!g_dl.last) g_dl.last = now;
  if (now - g_dl.last < 5000) return;
  if (g_dl.frames)
    patch::Log("gpu terrain 5s: %u frames drawn by it (%u by the game); per frame %.1f chunks, %.0f triangles, "
               "%.3f ms on the game thread (worst %.2f)",
               g_dl.frames, g_dl.game_frames, static_cast<double>(g_dl.chunks) / g_dl.frames,
               static_cast<double>(g_dl.tris) / g_dl.frames, g_dl.ms / g_dl.frames, g_dl.max_ms);
  const DWORD keep = now;
  g_dl = DrawLog();
  g_dl.last = keep;
}

}  // namespace

bool GpuTerrainActive() { return GetConfig().gpu_terrain && g_on && !g_failed && dd::GpuTerrainDrawable(); }

void GpuTerrainKeys() {
  static bool f7 = false;
  if (!GetConfig().gpu_terrain) return;
  if (KeyHit(VK_F7, f7) && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return;
    g_on = !g_on;
    patch::Log("gpu terrain: %s (Ctrl+F7)", g_on ? "ON" : "off, the game draws the terrain");
    dd::DebugNotice(g_on ? "GPU terrain ON" : "GPU terrain off");
  }
}

bool GpuTerrainDraw(void* root_v) {
  uint8_t* root = static_cast<uint8_t*>(root_v);
  if (!root || !GpuTerrainActive()) {
    Report(false);
    return false;
  }
  uint8_t* T = Rd<uint8_t*>(root, 0x34);
  if (T != g_built_T || root != g_built_root) {
    g_built_T = T;
    g_built_root = root;
    if (!Build(root)) {
      g_failed = true;
      Report(false);
      return false;
    }
  }
  const double t0 = Now();
  gt::DrawStats ds;
  const bool ok = dd::DrawGpuTerrain(g_mesh, &ds);
  const double ms = Now() - t0;
  if (!ok) {
    static bool said = false;
    if (!said) patch::Log("gpu terrain: the renderer could not draw it (Direct3D 9, or no device); the game draws the terrain");
    said = true;
    Report(false);
    return false;
  }
  if (ds.uploaded) patch::Log("gpu terrain: sent to the GPU in %.1f ms", ds.upload_ms);
  g_dl.chunks += ds.chunks_drawn;
  g_dl.tris += ds.tris_drawn;
  g_dl.ms += ms;
  if (ms > g_dl.max_ms) g_dl.max_ms = ms;
  Report(true);
  return true;
}
