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

#include <algorithm>
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

using SeekFn = int(__fastcall*)(void* stream, void* edx, int pos, int a, int b);
using ReadFn = int(__fastcall*)(void* stream, void* edx, void* dst, int size, int count);
const SeekFn kSeek = reinterpret_cast<SeekFn>(0x4622B0);
const ReadFn kRead = reinterpret_cast<ReadFn>(0x4625B0);

struct NodeInfo {
  int gx, gy, shift, level;
  float s10, s14;
  const uint8_t* tile;
  uint32_t texcode;
  bool has_subs;
  uint32_t sub[16];
  const NodeInfo* parent;
  int ix, iy;  // place in the parent
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
  int census_logged = 0;
  std::vector<std::pair<uint32_t, unsigned>> code_hist;  // codes of nodes above level 0
  std::vector<unsigned> mat_nodes;

  void CloseChunk() {
    if (open) {
      cur.first_range = static_cast<uint32_t>(m->ranges.size());
      for (size_t k = 0; k < pend.size(); ++k) {
        if (pend[k].empty()) continue;
        gt::Range r;
        r.first_index = static_cast<uint32_t>(m->idx.size());
        r.index_count = static_cast<uint32_t>(pend[k].size());
        r.material = static_cast<int32_t>(k) - 1;
        m->idx.insert(m->idx.end(), pend[k].begin(), pend[k].end());
        m->ranges.push_back(r);
        cur.index_count += r.index_count;
        pend[k].clear();
      }
      cur.range_count = static_cast<uint32_t>(m->ranges.size()) - cur.first_range;
      if (cur.index_count) m->chunks.push_back(cur);
    }
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
    ni.has_subs = false;
    for (int i = 0; i < 16; ++i)
      if ((sub[i] & 0xFFFE) != 0xFFFE) ni.has_subs = true;  // 0x47f6a0: a table only if one is set
    std::memcpy(ni.sub, sub, sizeof(sub));
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

  // ---- textures (the game's choice at full detail, 0x47d9f0): a node's
  // texture code is a shared texture (T+0x544[(int16)code >> 1]), a texture
  // streamed from the track file (bit 0), or 0xfffe/0xffff = 16 sub-block
  // textures (4x4 cells each, row-major), each code shared or streamed.
  // u = ((x - x0) * scale + offset) * 0.9375 + 0.03125 (0x47ede0), v from y;
  // a shared texture 64 wide repeats every cell (scale 1), otherwise it spans
  // the node (scale 1/16); sub-block textures use scale 1 from the
  // sub-block's corner.
  enum CodeKind { kNone, kShared, kStreamed, kSubs };
  static CodeKind Kind(uint32_t code) {
    if ((code & 0xFFFE) == 0xFFFE) return kSubs;
    if (code & 1) return kStreamed;
    return kShared;
  }
  static int SharedIndex(uint32_t code) { return static_cast<int16_t>(code & 0xFFFF) >> 1; }
  uint32_t nshared = 0;
  uint8_t* const* shared = nullptr;
  std::vector<int> mat_of_shared;  // shared texture -> material, -1 = not used yet
  std::vector<int> shared_of_mat;
  struct TexStats {
    unsigned node[4] = {}, sub[4] = {}, subs_missing = 0, bad_index = 0, w64 = 0, w128 = 0, wother = 0;
    unsigned from_parent = 0, from_parent_sub = 0, parent_none = 0;
  } ts;

  // Material of a shared texture code; -1 = draw it untextured.
  int Material(uint32_t code, float* uscale) {
    const int i = SharedIndex(code);
    if (i < 0 || static_cast<uint32_t>(i) >= nshared || !shared[i]) {
      ++ts.bad_index;
      return -1;
    }
    if (mat_of_shared[i] < 0) {
      mat_of_shared[i] = static_cast<int>(shared_of_mat.size());
      shared_of_mat.push_back(i);
      const int w = Rd<int32_t>(shared[i], 0x14);
      if (w == 64) ++ts.w64; else if (w == 128) ++ts.w128; else ++ts.wother;
    }
    *uscale = Rd<int32_t>(shared[i], 0x14) == 64 ? 1.0f : 1.0f / 16.0f;
    return mat_of_shared[i];
  }

  // Indices waiting for the chunk to close, by material (+1: 0 = untextured).
  std::vector<std::vector<uint16_t>> pend;
  int cur_mat = -1;
  void Add(uint32_t a, uint32_t b, uint32_t c) {
    const size_t k = static_cast<size_t>(cur_mat + 1);
    if (pend.size() <= k) pend.resize(k + 1);
    std::vector<uint16_t>& v = pend[k];
    v.push_back(static_cast<uint16_t>(a));
    v.push_back(static_cast<uint16_t>(b));
    v.push_back(static_cast<uint16_t>(c));
  }

  // One patch of n x n cells from grid point (x0, y0) of the node, with its
  // own vertices (material and uv), its cells that have no child, and skirts
  // on the edges that lie on the node's border.
  void Patch(const NodeInfo& ni, const bool* has_kid, const float (*H)[17], const float* X, const float* Z,
             const float (*N)[17][3], int x0, int y0, int n, int mat, float uscale, float uoff, float voff,
             float& ymin, float& ymax) {
    const int nv = (n + 1) * (n + 1) + 4 * (n + 1);
    Room(static_cast<unsigned>(nv));
    cur_mat = mat;
    const uint32_t base = cur.vertex_count;
    const uint32_t vfirst = static_cast<uint32_t>(m->verts.size());
    for (int y = y0; y <= y0 + n; ++y) {
      for (int x = x0; x <= x0 + n; ++x) {
        gt::Vertex v{};
        v.pos[0] = X[x];
        v.pos[1] = H[y][x];
        v.pos[2] = Z[y];
        v.pos[3] = 1;
        if (mat >= 0) {
          v.diffuse = log >= 2 ? Shade(N[y][x][0], N[y][x][1], N[y][x][2], ni.level, log) | 0xFF000000u
                               : 0xFFFFFFFFu;
          v.t0[0] = static_cast<float>(((x - x0) * uscale + uoff) * 0.9375 + 0.03125);
          v.t0[1] = static_cast<float>(((y - y0) * uscale + voff) * 0.9375 + 0.03125);
          // Detail texture (T+0xc3c), second uv. The game (mesh builder
          // 0x47f22c packs the vertex's cell index within its block, from the
          // block's own corner; 0x481cc2 multiplies by 1/12, 16/12 on level
          // 1 nodes) restarts the pattern at every block, so it is the same
          // size in the world on levels 0 and 1.
          const double ds = (ni.level == 1 ? 16.0 : 1.0) / 12.0;
          v.t1[0] = static_cast<float>((x - x0) * ds);
          v.t1[1] = static_cast<float>((y - y0) * ds);
        } else {
          v.diffuse = Shade(N[y][x][0], N[y][x][1], N[y][x][2], ni.level, log);
        }
        m->verts.push_back(v);
        if (H[y][x] < ymin) ymin = H[y][x];
        if (H[y][x] > ymax) ymax = H[y][x];
      }
    }
    const int w = n + 1;
    for (int cy = 0; cy < n; ++cy) {
      for (int cx = 0; cx < n; ++cx) {
        if (has_kid[(y0 + cy) * 16 + x0 + cx]) continue;
        const uint32_t a = base + cy * w + cx;
        Add(a, a + w, a + w + 1);
        Add(a, a + w + 1, a + 1);
        ++st.leaf_cells;
      }
    }
    // Skirts: a strip hanging down from each outer edge of the node where a
    // cell is drawn, so seams with finer or coarser neighbours show no sky.
    const float drop = std::fabs(X[1] - X[0]) * 1.5f + 1.0f;
    uint32_t vbase = base + w * w;
    // edge: start (local x, y), step; on the node border?
    const int edges[4][4] = {{0, 0, 1, 0}, {0, n, 1, 0}, {0, 0, 0, 1}, {n, 0, 0, 1}};
    const bool border[4] = {y0 == 0, y0 + n == 16, x0 == 0, x0 + n == 16};
    for (int e = 0; e < 4; ++e) {
      for (int k = 0; k <= n; ++k) {
        const int x = edges[e][0] + edges[e][2] * k, y = edges[e][1] + edges[e][3] * k;
        gt::Vertex v = m->verts[vfirst + y * w + x];
        v.pos[1] -= drop;
        m->verts.push_back(v);
      }
      if (border[e]) {
        for (int k = 0; k < n; ++k) {
          const int x = edges[e][0] + edges[e][2] * k, y = edges[e][1] + edges[e][3] * k;
          const int cx = x0 + (e == 3 ? n - 1 : (edges[e][2] ? k : 0));
          const int cy = y0 + (e == 1 ? n - 1 : (edges[e][3] ? k : 0));
          if (has_kid[cy * 16 + cx]) continue;
          const uint32_t t0 = base + y * w + x;
          const uint32_t t1 = base + (y + edges[e][3]) * w + (x + edges[e][2]);
          const uint32_t b0 = vbase + k, b1 = vbase + k + 1;
          Add(t0, b0, b1);
          Add(t0, b1, t1);
          ++st.skirt_quads;
        }
      }
      vbase += static_cast<uint32_t>(n + 1);
    }
    cur.vertex_count += static_cast<uint32_t>(nv);
    const float lo[3] = {std::fmin(X[x0], X[x0 + n]), ymin - drop, std::fmin(Z[y0], Z[y0 + n])};
    const float hi[3] = {std::fmax(X[x0], X[x0 + n]), ymax, std::fmax(Z[y0], Z[y0 + n])};
    for (int i = 0; i < 3; ++i) {
      if (lo[i] < cur.lo[i]) cur.lo[i] = lo[i];
      if (hi[i] > cur.hi[i]) cur.hi[i] = hi[i];
    }
  }

  void Emit(const NodeInfo& ni, const bool* has_kid) {
    unsigned leaves = 0;
    for (int i = 0; i < 256; ++i) leaves += has_kid[i] ? 0 : 1;
    if (!leaves || !ni.tile) return;
    ++st.nodes_drawn;
    if (ni.level >= 0 && ni.level < 16) ++st.by_level[ni.level];
    float H[17][17], X[17], Z[17], N[17][17][3];
    for (int i = 0; i < 17; ++i) {
      X[i] = Coord(ni.gx + i, ni.shift);
      Z[i] = Coord(ni.gy + i, ni.shift);
    }
    for (int y = 0; y < 17; ++y)
      for (int x = 0; x < 17; ++x) H[y][x] = Height(ni, y * 17 + x);
    const float cell = X[1] - X[0];
    const float inv = cell != 0 ? 1.0f / cell : 0.0f;
    for (int y = 0; y < 17; ++y) {
      for (int x = 0; x < 17; ++x) {
        const int x0 = x > 0 ? x - 1 : x, x1 = x < 16 ? x + 1 : x;
        const int y0 = y > 0 ? y - 1 : y, y1 = y < 16 ? y + 1 : y;
        const float dx = (H[y][x1] - H[y][x0]) * inv / static_cast<float>(x1 - x0);
        const float dz = (H[y1][x] - H[y0][x]) * inv / static_cast<float>(y1 - y0);
        const float l = 1.0f / std::sqrt(dx * dx + 1 + dz * dz);
        N[y][x][0] = -dx * l;
        N[y][x][1] = l;
        N[y][x][2] = -dz * l;
      }
    }
    float ymin = 3e38f, ymax = -3e38f;
    const CodeKind k = Kind(ni.texcode);
    ++ts.node[k];
    if (k == kSubs && ni.has_subs) {
      for (int s = 0; s < 16; ++s) {
        const int sx = (s & 3) * 4, sy = (s >> 2) * 4;
        bool any = false;
        for (int cy = 0; cy < 4 && !any; ++cy)
          for (int cx = 0; cx < 4; ++cx)
            if (!has_kid[(sy + cy) * 16 + sx + cx]) any = true;
        if (!any) continue;
        const uint32_t sc = ni.sub[s];
        const CodeKind sk = (sc & 0xFFFE) == 0xFFFE ? kNone : Kind(sc);
        ++ts.sub[sk];
        float us = 1;
        const int mat = sk == kShared ? Material(sc, &us) : -1;
        Patch(ni, has_kid, H, X, Z, N, sx, sy, 4, mat, 1.0f, 0, 0, ymin, ymax);
      }
    } else if (k == kShared) {
      float us = 1;
      const int mat = Material(ni.texcode, &us);
      Patch(ni, has_kid, H, X, Z, N, 0, 0, 16, mat, us, 0, 0, ymin, ymax);
    } else {
      // No texture of its own (0x47db09): the parent's, if shared, over the
      // part this node covers (1/16 of it); else the parent's sub-block
      // texture over this node's quarter of it.
      if (k == kSubs) ++ts.subs_missing;
      const NodeInfo* P = ni.parent;
      int mat = -1;
      float us = 1, sc = 0, uo = 0, vo = 0;
      // Up close the game's choice falls to case 1 (0x47db2e, the parent's
      // sub-block texture: 4x4 blocks share it), far away to case 0 (the
      // parent's own texture over 16x16 blocks).
      const uint32_t psub = (P && P->has_subs) ? P->sub[(ni.iy >> 2) * 4 + (ni.ix >> 2)] : 0xFFFFu;
      if ((psub & 0xFFFE) != 0xFFFE && !(psub & 1)) {
        mat = Material(psub, &us);
        sc = 1.0f / 64, uo = (ni.ix & 3) / 4.0f, vo = (ni.iy & 3) / 4.0f;
        ++ts.from_parent_sub;
      } else if (P && Kind(P->texcode) == kShared) {
        mat = Material(P->texcode, &us);
        sc = 1.0f / 256, uo = ni.ix / 16.0f, vo = ni.iy / 16.0f;
        ++ts.from_parent;
      } else {
        ++ts.parent_none;
      }
      Patch(ni, has_kid, H, X, Z, N, 0, 0, 16, mat, sc, uo, vo, ymin, ymax);
    }
    if (ni.level >= 1 && log >= 1) {
      const uint32_t c = ni.texcode & 0xFFFF;
      for (auto& h : code_hist)
        if (h.first == c) {
          ++h.second;
          return;
        }
      if (code_hist.size() < 64) code_hist.push_back({c, 1u});
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
          VisitMem(c, depth + 1, &ni, i);
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
            c.parent = &ni;
            c.ix = i & 15;
            c.iy = i >> 4;
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

  void VisitMem(uint8_t* n, int depth, const NodeInfo* parent = nullptr, int index = 0) {
    NodeInfo ni{};
    ni.parent = parent;
    ni.ix = index & 15;
    ni.iy = index >> 4;
    ni.gx = Rd<uint16_t>(n, 0x24);
    ni.gy = Rd<uint16_t>(n, 0x26);
    ni.level = Rd<uint8_t>(n, 0x28);
    ni.shift = Rd<uint8_t>(n, 0x29);
    ni.s10 = Rd<float>(n, 0x10);
    ni.s14 = Rd<float>(n, 0x14);
    ni.tile = Rd<const uint8_t*>(n, 0x0C);
    const uint8_t* tex = Rd<const uint8_t*>(n, 0x3C);
    ni.texcode = tex ? Rd<uint32_t>(tex, 0x0C) : 0xFFFFFFFFu;
    const uint8_t* subs = tex ? Rd<const uint8_t*>(tex, 0x38) : nullptr;
    ni.has_subs = subs != nullptr;
    if (subs)
      for (int i = 0; i < 16; ++i) ni.sub[i] = Rd<uint32_t>(subs, i * 0x2C);
    if (log >= 1 && subs && census_logged < 3) {
      ++census_logged;
      char hx[0x40 * 3 + 1], hs[0x2C * 3 + 1];
      for (int i = 0; i < 0x40; ++i) std::snprintf(hx + i * 3, 4, "%02x ", tex[i]);
      for (int i = 0; i < 0x2C; ++i) std::snprintf(hs + i * 3, 4, "%02x ", subs[i]);
      patch::Log("gpu terrain: node %u,%u level %u texture data: %s; sub-block 0: %s", ni.gx, ni.gy, ni.level, hx, hs);
    }
    Visit(ni, Rd<uint8_t* const*>(n, 0x04), Rd<uint32_t>(n, 0x2C), depth);
  }
};

// ---- the game's own near/far split (GpuTerrainLog, game terrain drawn)
// For each visible node of the game's tree: its texel measure (texture data
// +0x34, 0x47d9f0) against T+0xca4 decides whether the game draws it "near"
// (white, detail texture) or "far" (colours T+0xc9c / T+0xca0, no detail).
// Logs, per level, how far from the camera each kind reaches.
struct ProbeLevel {
  unsigned near_n = 0, far_n = 0;
  float near_max = 0, far_min = 1e30f;
  std::vector<float> k;  // measure * distance
  unsigned scale_n[4] = {};
};
void ProbeWalk(uint8_t* n, const float* eye, float ca4, ProbeLevel* lv, int depth) {
  if (!n || depth > 6) return;
  const uint32_t vis = Rd<uint32_t>(n, 0x40);
  if (!(vis & 1)) return;
  const int level = Rd<uint8_t>(n, 0x28);
  const uint8_t* tex = Rd<const uint8_t*>(n, 0x3C);
  const uint8_t* D = Rd<const uint8_t*>(n, 0x38);
  if (tex && level < 4) {
    const float m = Rd<float>(tex, 0x34);
    const float cx = Rd<float>(n, 0x44) - eye[0], cy = Rd<float>(n, 0x48) - eye[1], cz = Rd<float>(n, 0x4C) - eye[2];
    const float d = std::sqrt(cx * cx + cy * cy + cz * cz);
    ProbeLevel& L = lv[level];
    if (ca4 >= m) {
      ++L.far_n;
      if (d < L.far_min) L.far_min = d;
    } else {
      ++L.near_n;
      if (d > L.near_max) L.near_max = d;
    }
    if (L.k.size() < 8192) L.k.push_back(m * d);
    if (D) {
      const float sc = Rd<float>(D, 0x160);
      L.scale_n[sc < 0.005f ? 0 : sc < 0.03f ? 1 : sc < 0.5f ? 2 : 3]++;
    }
  }
  uint8_t* const* kids = Rd<uint8_t* const*>(n, 0x04);
  if (kids)
    for (int i = 0; i < 256; ++i)
      if (kids[i]) ProbeWalk(kids[i], eye, ca4, lv, depth + 1);
}

// ---- state

gt::Mesh g_mesh;
uint8_t* g_built_T = nullptr;
uint8_t* g_built_root = nullptr;
bool g_failed = false;
bool g_on = true;  // Ctrl+F7

struct DrawLog {
  unsigned frames = 0, chunks = 0, tris = 0, game_frames = 0, draws = 0;
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
  b.nshared = Rd<uint32_t>(b.T, 0x540);
  if (b.nshared > 65536) b.nshared = 0;
  b.shared = reinterpret_cast<uint8_t* const*>(b.T + 0x544);
  b.mat_of_shared.assign(b.nshared, -1);
  b.VisitMem(root, 0);
  b.CloseChunk();
  // The renderer's texture for each shared texture used: the game's texture
  // object sets itself on stage 0 (vtable +0x4c, as 0x47e26f does).
  using SetFn = void(__fastcall*)(void* obj, void* edx);
  m.textures.assign(b.shared_of_mat.size(), nullptr);
  unsigned no_tex = 0;
  for (size_t i = 0; i < b.shared_of_mat.size(); ++i) {
    uint8_t* obj = b.shared[b.shared_of_mat[i]];
    void** vt = Rd<void**>(obj, 0);
    reinterpret_cast<SetFn>(vt[0x4C / 4])(obj, nullptr);
    m.textures[i] = dd::GpuTerrainCurrentTexture();
    if (!m.textures[i]) ++no_tex;
    if (b.log >= 1 && i < 6) {
      char d[160];
      dd::GpuTerrainDescribeTexture(m.textures[i], d, sizeof(d));
      patch::Log("gpu terrain: material %zu = shared texture %d (object %p: +14 %d +6c %d +68 %08x +84 %.4f +88 %.4f "
                 "+8c %.4f) -> %s",
                 i, b.shared_of_mat[i], static_cast<void*>(obj), Rd<int32_t>(obj, 0x14), Rd<int32_t>(obj, 0x6C),
                 Rd<uint32_t>(obj, 0x68), Rd<float>(obj, 0x84), Rd<float>(obj, 0x88), Rd<float>(obj, 0x8C), d);
    }
  }
  // The detail texture the game lays over the terrain near the camera
  // (T+0xc3c, its surface at +0x74; used when 0x57a8dc is set).
  {
    uint8_t* dobj = Rd<uint8_t*>(b.T, 0xC3C);
    const int on = *reinterpret_cast<const int*>(0x57A8DC);
    const int multi = Rd<int>(*reinterpret_cast<uint8_t* const*>(0x5743AC), 0x2D0);
    void* dsurf = dobj ? Rd<void*>(dobj, 0x74) : nullptr;
    if (dsurf && on && GetConfig().gpu_terrain_detail) {
      m.detail = dsurf;
      dd::GpuTerrainAddRefTexture(dsurf);
    }
    // The game lays it only on blocks whose texel measure (0x47d9f0: 16 cells
    // * T+0x70 / distance / 0.25 for a block on its parent's sub-block
    // texture) is above T+0xca4: out to T+0x70 * 64 / ca4.
    const float t70 = static_cast<float>(Rd<int32_t>(b.T, 0x70));
    const float ca4 = static_cast<float>(Rd<int32_t>(b.T, 0xCA4));
    const float game_reach = (t70 > 0 && ca4 > 0) ? t70 * 64.0f / ca4 : 0;  // logged only
    // That test doesn't match what the game shows: its detail covers the
    // terrain as far as level 0 and 1 blocks are drawn (StOo, gterr-6
    // screenshots), which with full detail everywhere is all of it. No fade
    // unless the ini sets a reach.
    const float reach = GetConfig().gpu_terrain_detail_reach > 0 ? GetConfig().gpu_terrain_detail_reach : 0.0f;
    m.detail_fade[0] = reach * 0.75f;
    m.detail_fade[1] = reach;
    // The game's far blocks (GpuTerrainFar): measured with GpuTerrainLog
    // (gterr-10: level 0 blocks near out to ~930 units at T+70 786, ca4 256,
    // scale 4.5), which is the texel test 64 * T+70 / distance (in grid
    // units) against ca4, plus about half a block.
    const float scale = Rd<float>(b.T, 0x40);
    if (GetConfig().gpu_terrain_far && ca4 > 0 && scale > 0) {
      m.far_k = 64.0f * scale / ca4 * 1.02f;
      m.far_diffuse = Rd<uint32_t>(b.T, 0xC9C);
      m.far_specular = Rd<uint32_t>(b.T, 0xCA0);
    }
    patch::Log("gpu terrain: far blocks %s: from %.3f x focal length (%.0f units now), diffuse %08x specular %08x",
               m.far_k > 0 ? "as the game's" : "off", m.far_k, m.far_k * t70, m.far_diffuse, m.far_specular);
    patch::Log("gpu terrain: detail reach %.0f (0 = everywhere; ini %.0f; the game's texel test would give %.0f from "
               "T+70 %d, ca4 %d); detail uv restarts per block, 1/12 per cell (16/12 on level 1)",
               reach, GetConfig().gpu_terrain_detail_reach, game_reach, Rd<int32_t>(b.T, 0x70), Rd<int32_t>(b.T, 0xCA4));
    char d[160];
    dd::GpuTerrainDescribeTexture(dsurf, d, sizeof(d));
    patch::Log("gpu terrain: detail texture object %p surface %s, game's detail switch %d, game's mode %s; %s",
               static_cast<void*>(dobj), d, on, multi ? "two passes" : "one pass, two stages",
               m.detail ? "drawn" : "not drawn");
  }
  const Builder::TexStats& t = b.ts;
  patch::Log("gpu terrain: textures: %u shared in the track; node codes shared %u, streamed %u, sub-blocks %u (%u "
             "without a sub-block table); sub-block codes shared %u, streamed %u, none %u; %u bad shared numbers; "
             "%zu shared textures used (64 wide %u, 128 wide %u, other %u), %u without a renderer texture; nodes "
             "with no texture of their own: parent's %u, parent's sub-block %u, none %u; colours c9c %08x ca0 %08x, "
             "cb0 %d, ca4 %d",
             b.nshared, t.node[Builder::kShared], t.node[Builder::kStreamed], t.node[Builder::kSubs], t.subs_missing,
             t.sub[Builder::kShared], t.sub[Builder::kStreamed], t.sub[Builder::kNone], t.bad_index,
             b.shared_of_mat.size(), t.w64, t.w128, t.wother, no_tex, t.from_parent, t.from_parent_sub,
             t.parent_none, Rd<uint32_t>(b.T, 0xC9C), Rd<uint32_t>(b.T, 0xCA0), Rd<int32_t>(b.T, 0xCB0),
             Rd<int32_t>(b.T, 0xCA4));
  if (b.log >= 1) {
    char h[1024];
    int hn = 0;
    for (size_t i = 0; i < b.code_hist.size() && hn < 900; ++i)
      hn += std::snprintf(h + hn, sizeof(h) - hn, " %04x:%u", b.code_hist[i].first, b.code_hist[i].second);
    h[hn] = 0;
    patch::Log("gpu terrain: texture codes of nodes above level 0 (code:count):%s", h);
  }
  const double ms = Now() - t0;
  if (m.chunks.empty()) {
    patch::Log("gpu terrain: no terrain found under the root node (%u nodes); the game draws the terrain",
               b.st.nodes_mem);
    for (void* tx : m.textures) dd::GpuTerrainReleaseTexture(tx);
    dd::GpuTerrainReleaseTexture(m.detail);
    return false;
  }
  m.generation = g_mesh.generation + 1;
  for (void* t : g_mesh.textures) dd::GpuTerrainReleaseTexture(t);
  dd::GpuTerrainReleaseTexture(g_mesh.detail);
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
      patch::Log("gpu terrain: chunk %zu: %u vertices, %u triangles in %u textures, box %.0f,%.0f,%.0f - "
                 "%.0f,%.0f,%.0f",
                 i, c.vertex_count, c.index_count / 3, c.range_count, c.lo[0], c.lo[1], c.lo[2], c.hi[0], c.hi[1],
                 c.hi[2]);
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
    patch::Log("gpu terrain 5s: %u frames drawn by it (%u by the game); per frame %.1f chunks, %.1f draws, %.0f "
               "triangles, %.3f ms on the game thread (worst %.2f)",
               g_dl.frames, g_dl.game_frames, static_cast<double>(g_dl.chunks) / g_dl.frames,
               static_cast<double>(g_dl.draws) / g_dl.frames, static_cast<double>(g_dl.tris) / g_dl.frames,
               g_dl.ms / g_dl.frames, g_dl.max_ms);
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

bool GpuTerrainPrepare(void* root_v) {
  uint8_t* root = static_cast<uint8_t*>(root_v);
  if (!root || !GpuTerrainActive()) return false;
  uint8_t* T = Rd<uint8_t*>(root, 0x34);
  if (T != g_built_T || root != g_built_root) {
    g_built_T = T;
    g_built_root = root;
    if (!Build(root)) {
      g_failed = true;
      return false;
    }
  }
  return true;
}

bool GpuTerrainDraw(void* root_v) {
  if (!GpuTerrainPrepare(root_v)) {
    Report(false);
    return false;
  }
  const double t0 = Now();
  gt::DrawStats ds;
  if (g_mesh.far_k > 0 && g_built_T) {
    const float focal = static_cast<float>(Rd<int32_t>(g_built_T, 0x70));
    if (focal != g_mesh.focal && GetConfig().gpu_terrain_log)
      patch::Log("gpu terrain: focal length %.0f: far blocks from %.0f units", focal, g_mesh.far_k * focal);
    g_mesh.focal = focal;
    g_mesh.far_dist = g_mesh.far_k * focal;
  }
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
  g_dl.draws += ds.draws;
  g_dl.ms += ms;
  if (ms > g_dl.max_ms) g_dl.max_ms = ms;
  Report(true);
  return true;
}

void GpuTerrainProbeGame(void* root_v, const float* eye) {
  uint8_t* root = static_cast<uint8_t*>(root_v);
  if (!root || !eye) return;
  uint8_t* T = Rd<uint8_t*>(root, 0x34);
  if (!T) return;
  const float ca4 = static_cast<float>(Rd<int32_t>(T, 0xCA4));
  ProbeLevel lv[4];
  ProbeWalk(root, eye, ca4, lv, 0);
  patch::Log("gpu terrain: game's terrain near/far split: eye (%.0f %.0f %.0f), ca4 %.0f, T+70 %d, c9c %08x ca0 %08x",
             eye[0], eye[1], eye[2], ca4, Rd<int32_t>(T, 0x70), Rd<uint32_t>(T, 0xC9C), Rd<uint32_t>(T, 0xCA0));
  for (int l = 0; l < 4; ++l) {
    ProbeLevel& L = lv[l];
    if (!L.near_n && !L.far_n) continue;
    std::sort(L.k.begin(), L.k.end());
    const float kmed = L.k.empty() ? 0 : L.k[L.k.size() / 2];
    const float kmin = L.k.empty() ? 0 : L.k.front(), kmax = L.k.empty() ? 0 : L.k.back();
    patch::Log("gpu terrain:   level %d: %u near (out to %.0f), %u far (from %.0f); measure*distance min %.0f median "
               "%.0f max %.0f; texture scale 1/256 %u, 1/64 %u, 1/16 %u, 1 %u",
               l, L.near_n, L.near_max, L.far_n, L.far_n ? L.far_min : 0.0f, kmin, kmed, kmax, L.scale_n[0],
               L.scale_n[1], L.scale_n[2], L.scale_n[3]);
  }
}
