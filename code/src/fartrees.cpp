// FarTrees: the far tree billboards drawn on the graphics card.
//
// With a long view distance the game hands every tree in view through its
// vegetation pass (0x45b890: level of detail, crossfade, lists) and then
// builds a camera-facing billboard for each one on the CPU (0x45be20), 120
// at a time. At ViewDistance=10 on Baja that is ~13,000 trees a frame and
// about a fifth of the game thread.
//
// A far billboard is a plain quad whose shape, texture coordinates and
// height come from the tree alone; only its facing (the camera's direction)
// and its colour (the sun on the billboard, one colour for all far trees)
// change from frame to frame. So: each far tree's numbers are written down
// once (12 floats), kept in a buffer on the graphics card, and one draw
// makes all far trees from them, with the game's own culling (distance
// limit, too small on screen) done in the vertex shader. The game's passes
// never see them: FastObjectWalk's vegetation pass (fixes_walk.cpp) leaves
// them out.
//
// "Far": the game's depth key (vfunc 0, obj+6, times mgr+0x5bc) past
// FarTreeDistance (default: one and a half times the 3D tree distance, so
// the game frees a tree's 3D model before we take it over), of the
// vegetation layer and the billboard texture, and not one the game would
// draw as its bent six-vertex billboard (close up). The shader keeps the
// same rule with a small overlap, so a tree on the edge is drawn twice
// rather than not at all.
//
// The draw goes in where the builder sets the billboard state (the call of
// 0x45bba0 at 0x45c51e): same texture, blending, fog and depth state as the
// game's own billboards, and before them (the far ones first, as the game
// sorts). When no near billboard is left, the builder would skip that part
// (0x45c4e3); then it is run anyway for the far ones.
//
// The billboard builder, per tree (T = the tree's type, mgr+0x58 + 4*obj+0x12):
//   H = lerp(T+0x18c, T+0x190 / T+0x194, obj+0x15)   (0x456db0)
//   R = lerp(T+0x180, T+0x184 / T+0x188, obj+0x14)   (0x456d50), the height
//   k = 2H / (T+0x1bc - T+0x1b8); left = (T+0x1c8 - T+0x1b8) k, right = (T+0x1bc - T+0x1c8) k
//   p = obj+0xc,0xe,0x10 (words) * mgr+0x5a8; bottom = p.y + R * T+0x1d4, top = bottom + R
//   v0 = (p.x + right * cs, bottom, p.z - right * cc), v1 = (p.x - left * cs, bottom, p.z + left * cc),
//   v2 = v1 at the top, v3 = v0 at the top; (cc, cs) the camera's direction, normalised
//   u: v0 v3 T+0x1bc, v1 v2 T+0x1b8; v: v2 v3 T+0x1c0, v0 v1 T+0x1c4; triangles (0 1 2) (0 2 3)
//   colour: the sun on a billboard facing the camera (builder's local [esp+0x20]), alpha 255
//   culled: depth >= the LOD entry's +0xc; H * C+0x198 / depth <= 2
//   bent (6 vertices) when LOD +0x10 and ((LOD +0x14 and depth < 20) or depth * 15 / W+0xc < H)
#include "fartrees.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "patch.h"
#include "prof.h"

namespace {

constexpr uintptr_t kMgrPtr = 0x5A100C;     // the vegetation manager
constexpr uintptr_t kLodTable = 0x5A1064;   // LOD entries, 32 bytes, index mgr+0x5c0
constexpr uintptr_t kCamMatrix = 0x5A1050;  // the camera matrix the depth keys use
constexpr uintptr_t kTreeVtable = 0x557634;
constexpr uintptr_t kGridPtr = 0x690CE4;
constexpr uintptr_t kCountCheck = 0x45C4E3;  // cmp [esi+5A0h],ebx; je 45CB4Fh
constexpr uintptr_t kStateCall = 0x45C51E;   // call 45BBA0h (billboard state)

template <typename T>
T& At(const void* p, size_t off) {
  return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(p) + off);
}
template <typename T>
T Mem(uintptr_t a) {
  return *reinterpret_cast<const T*>(a);
}

// 0x456d50 / 0x456db0: a size between three values by a byte.
float Lerp3(const void* type, size_t base, uint8_t b) {
  const float k = Mem<float>(0x55762C);  // 1/128
  const float a0 = At<float>(type, base), a1 = At<float>(type, base + 4), a2 = At<float>(type, base + 8);
  if (b < 0x80) return a0 - (a0 - a1) * static_cast<float>(b) * k;
  return (a2 - a0) * static_cast<float>(255 - b) * k + a0;
}

struct TreeNumbers {
  float f[12];
  float h;
};

TreeNumbers Numbers(const void* mgr, const void* obj, const void* type) {
  TreeNumbers n{};
  const float s = At<float>(mgr, 0x5A8);
  const float px = static_cast<float>(At<uint16_t>(obj, 0xC)) * s;
  const float py = static_cast<float>(At<uint16_t>(obj, 0xE)) * s;
  const float pz = static_cast<float>(At<uint16_t>(obj, 0x10)) * s;
  const float h = Lerp3(type, 0x18C, At<uint8_t>(obj, 0x15));
  const float r = Lerp3(type, 0x180, At<uint8_t>(obj, 0x14));
  const float ua = At<float>(type, 0x1C8), ub = At<float>(type, 0x1B8), uc = At<float>(type, 0x1BC);
  const float k = 2 * h / (uc - ub);
  const float bottom = py + r * At<float>(type, 0x1D4);
  n.f[0] = px;
  n.f[1] = pz;
  n.f[2] = bottom;
  n.f[3] = bottom + r;
  n.f[4] = (ua - ub) * k;  // left
  n.f[5] = (uc - ua) * k;  // right
  n.f[6] = h;
  n.f[7] = 0;
  n.f[8] = uc;
  n.f[9] = ub;
  n.f[10] = At<float>(type, 0x1C0);
  n.f[11] = At<float>(type, 0x1C4);
  n.h = h;
  return n;
}

// What a tree was when written down (to notice reused memory).
struct Ident {
  uint16_t x, y, z;
  uint8_t type, b14, b15, pad;
  bool operator==(const Ident& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
Ident IdentOf(const void* obj) {
  Ident i{};
  i.x = At<uint16_t>(obj, 0xC);
  i.y = At<uint16_t>(obj, 0xE);
  i.z = At<uint16_t>(obj, 0x10);
  i.type = At<uint8_t>(obj, 0x12);
  i.b14 = At<uint8_t>(obj, 0x14);
  i.b15 = At<uint8_t>(obj, 0x15);
  return i;
}

// The trees written down: numbers (to the GPU), identities, and a hash from
// object to index.
struct Registry {
  std::vector<float> nums;  // 12 per tree
  std::vector<Ident> ids;
  std::vector<const void*> objs;
  std::vector<uint32_t> table;  // open addressing, index + 1 (0 = empty)
  size_t dirty_from = 0;        // first tree not yet sent

  size_t Count() const { return objs.size(); }
  void Clear() {
    nums.clear();
    ids.clear();
    objs.clear();
    table.assign(table.size() ? table.size() : 1 << 16, 0);
    dirty_from = 0;
  }
  static size_t Hash(const void* p) {
    uint32_t v = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)) >> 2;
    v ^= v >> 15;
    v *= 0x2C1B3C6Du;
    v ^= v >> 12;
    return v;
  }
  void Grow() {
    std::vector<uint32_t> t(table.size() * 2, 0);
    const size_t mask = t.size() - 1;
    for (size_t i = 0; i < objs.size(); ++i) {
      size_t h = Hash(objs[i]) & mask;
      while (t[h]) h = (h + 1) & mask;
      t[h] = static_cast<uint32_t>(i + 1);
    }
    table.swap(t);
  }
  // Index of `obj`, written down now if new (or its memory now holds another tree).
  size_t Find(const void* mgr, const void* obj, const void* type, bool* added) {
    if (table.empty()) table.assign(1 << 16, 0);
    const size_t mask = table.size() - 1;
    size_t h = Hash(obj) & mask;
    const Ident id = IdentOf(obj);
    while (table[h]) {
      const size_t i = table[h] - 1;
      if (objs[i] == obj) {
        if (!(ids[i] == id)) {
          const TreeNumbers n = Numbers(mgr, obj, type);
          std::memcpy(&nums[i * 12], n.f, sizeof(n.f));
          ids[i] = id;
          dirty_from = std::min(dirty_from, i);
          *added = true;
        }
        return i;
      }
      h = (h + 1) & mask;
    }
    const size_t i = objs.size();
    const TreeNumbers n = Numbers(mgr, obj, type);
    nums.insert(nums.end(), n.f, n.f + 12);
    ids.push_back(id);
    objs.push_back(obj);
    table[h] = static_cast<uint32_t>(i + 1);
    *added = true;
    if (objs.size() * 2 > table.size()) Grow();
    return i;
  }
} g_reg;

// The far trees in the order the walk handed them over last pass, with what
// each needs here (identity, H). The walk visits them in nearly the same
// order every frame, so most trees are found by reading this list straight
// through instead of a hash lookup (several cache misses per tree with tens
// of thousands of trees).
struct Hot {
  const void* obj;
  uint32_t idx;
  Ident id;
  float h;
};
struct Sequence {
  std::vector<Hot> prev, cur;
  std::vector<uint32_t> where;  // tree index -> its place in prev (+1; 0 = not there)
  size_t pos = 0;
  void Clear() {
    prev.clear();
    cur.clear();
    where.clear();
    pos = 0;
  }
  // A new pass: this pass's list becomes the one to follow.
  void Start() {
    prev.swap(cur);
    cur.clear();
    pos = 0;
    if (where.size() < g_reg.Count()) where.resize(g_reg.Count() + g_reg.Count() / 4 + 64, 0);
    std::fill(where.begin(), where.end(), 0u);
    for (size_t k = 0; k < prev.size(); ++k)
      if (prev[k].idx < where.size()) where[prev[k].idx] = static_cast<uint32_t>(k + 1);
  }
} g_seq;

// FarTreeCells: grid cells holding nothing but far-tree candidates (trees
// of the vegetation layer and the billboard texture), noted the first time
// they are walked: a signature of their bucket chain (to notice anything
// moving in or out), the box their trees stand in and their largest H. A
// cell whose nearest corner is past the far distance (with a margin) and
// too far for any of its trees to be a bent billboard is left out of the
// walk altogether: its trees are all drawn here anyway, and are already
// written down.
struct CellInfo {
  uint32_t sig = 0, objs = 0;
  bool pure = false;
  float x0 = 0, x1 = 0, z0 = 0, z1 = 0, hmax = 0;
};
std::unordered_map<const void*, CellInfo> g_cells;

// This pass's rule (FarTreesPassStart).
struct Pass {
  bool ok = false;
  const void* mgr = nullptr;
  const void* tex = nullptr;  // the billboard texture (T+0x1e0)
  uint8_t layer = 0;
  float unit = 0;      // key -> depth
  float far_d = 0;     // FarTreeDistance in use
  float six_ac = 0;
  bool six_a8 = false, six_84 = false;
  float l0 = 0;        // the 3D tree distance (LOD +0)
} g_pass;

bool g_disabled = false;  // the draw failed: the game draws all trees again
const void* g_seen_mgr = nullptr;
const void* g_seen_types = nullptr;
const void* g_seen_grid = nullptr;
bool g_skipped_this_frame = false;
bool g_logged_setup = false;

struct Stats {
  unsigned seq_hits = 0, seq_misses = 0, seq_resyncs = 0;
  uint64_t cells_seen = 0, cells_pure = 0, cells_skipped = 0, cell_trees_skipped = 0, cells_built = 0;
  unsigned frames = 0, passes = 0, skipped = 0, handed = 0, near_trees = 0, six = 0, other_tex = 0, draws = 0,
           missed = 0, resets = 0, added = 0, draw_fail = 0, empty_runs = 0;
  double draw_ms = 0;
  // FarTreeLog check against the game's billboards
  unsigned chk_frames = 0, chk_quads = 0, chk_matched = 0, chk_unmatched = 0, chk_bad = 0, chk_six = 0,
           chk_pred_emit = 0, chk_pred_culled_but_drawn = 0, chk_overlap = 0, chk_six_matched = 0;
  float chk_max_pos = 0, chk_max_uv = 0;
  unsigned chk_col_bad = 0;
} g_st;
double g_st_since = 0;
LARGE_INTEGER g_freq;

double Now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

const void* TypeOf(const void* mgr, const void* obj) {
  return At<const void*>(mgr, 0x58 + 4 * static_cast<size_t>(At<uint8_t>(obj, 0x12)));
}

const uint8_t* LodEntry(const void* mgr) {
  return Mem<const uint8_t*>(kLodTable) + 32 * static_cast<size_t>(At<uint32_t>(mgr, 0x5C0));
}

bool SixVertex(float depth, float h) {
  return g_pass.six_a8 && ((g_pass.six_84 && depth < Mem<float>(0x555C14)) || depth * g_pass.six_ac < h);
}

// The frame's numbers for the shader and for the check (FarTreesStateDone).
FarTreeFrame g_frame{};
bool g_frame_ok = false;

void MaybeLog();

}  // namespace

void FarTreesPassStart() {
  g_pass.ok = false;
  if (!GetConfig().far_trees || g_disabled) return;
  const void* mgr = Mem<const void*>(kMgrPtr);
  if (!mgr || !Mem<const uint8_t*>(kLodTable) || !dd::FarTreesDrawable()) return;
  const void* proto = At<const void*>(mgr, 0x38);  // the tree the builder takes the texture from
  if (!proto) return;
  const void* ptype = TypeOf(mgr, proto);
  if (!ptype) return;
  // A new race, track or grid: the trees written down are gone.
  const void* types0 = At<const void*>(mgr, 0x58);
  const void* grid = Mem<const void*>(kGridPtr);
  if (mgr != g_seen_mgr || types0 != g_seen_types || grid != g_seen_grid) {
    if (g_reg.Count()) ++g_st.resets;
    g_reg.Clear();
    g_seq.Clear();
    g_cells.clear();
    g_seen_mgr = mgr;
    g_seen_types = types0;
    g_seen_grid = grid;
    g_logged_setup = false;
  }
  const uint8_t* lod = LodEntry(mgr);
  const void* w = At<const void*>(mgr, 0x18);
  if (!w) return;
  const int32_t wc = At<int32_t>(w, 0xC);
  g_pass.mgr = mgr;
  g_pass.tex = At<const void*>(ptype, 0x1E0);
  g_pass.layer = At<uint8_t>(mgr, 0x599);
  g_pass.unit = At<float>(mgr, 0x5BC);
  g_pass.l0 = static_cast<float>(At<int32_t>(lod, 0));
  g_pass.six_a8 = At<int32_t>(lod, 0x10) != 0;
  g_pass.six_84 = At<int32_t>(lod, 0x14) != 0;
  g_pass.six_ac = wc ? static_cast<float>(Mem<double>(0x5576D0) / wc) : 0.0f;
  const float min_far = g_pass.l0 * 1.5f;
  g_pass.far_d = std::max(GetConfig().far_tree_distance, min_far);
  if (!(g_pass.far_d > 0) || !(g_pass.unit > 0)) return;
  g_pass.ok = true;
  g_seq.Start();
  ++g_st.passes;
  if (GetConfig().far_tree_log && !g_logged_setup) {
    g_logged_setup = true;
    const int32_t* l = reinterpret_cast<const int32_t*>(lod);
    patch::Log("far trees: LOD entry %u: %d %d %d %d %d %d %d %d; 3D trees to %.0f, far trees from %.0f "
               "(FarTreeDistance %.0f); depth unit %.4f, key scale %.2f, W+0xc %d, layer %u, texture %p",
               At<uint32_t>(mgr, 0x5C0), l[0], l[1], l[2], l[3], l[4], l[5], l[6], l[7], g_pass.l0, g_pass.far_d,
               GetConfig().far_tree_distance, g_pass.unit, At<float>(mgr, 0x5B8), wc, g_pass.layer, g_pass.tex);
  }
}

bool FarTreeSkip(void* obj) {
  if (!g_pass.ok) return false;
  if (Mem<uintptr_t>(reinterpret_cast<uintptr_t>(obj)) != kTreeVtable) return false;
  if (At<uint8_t>(obj, 8) != g_pass.layer) return false;
  const float depth = static_cast<float>(At<uint16_t>(obj, 6)) * g_pass.unit;
  if (!(depth > g_pass.far_d)) {
    ++g_st.near_trees;
    return false;
  }
  const void* type = TypeOf(g_pass.mgr, obj);
  if (!type || At<const void*>(type, 0x1E0) != g_pass.tex) {
    ++g_st.other_tex;
    return false;
  }
  // Next in last pass's order (the usual case), else the hash lookup, and
  // carry on from where that tree was last pass.
  const Ident id = IdentOf(obj);
  Hot hot;
  Sequence& q = g_seq;
  if (q.pos < q.prev.size() && q.prev[q.pos].obj == obj && q.prev[q.pos].id == id) {
    hot = q.prev[q.pos++];
    ++g_st.seq_hits;
  } else {
    bool added = false;
    const size_t i = g_reg.Find(g_pass.mgr, obj, type, &added);
    if (added) ++g_st.added;
    hot = Hot{obj, static_cast<uint32_t>(i), id, g_reg.nums[i * 12 + 6]};
    ++g_st.seq_misses;
    if (i < q.where.size() && q.where[i]) {
      q.pos = q.where[i];  // the one after it
      ++g_st.seq_resyncs;
    }
  }
  q.cur.push_back(hot);
  if (SixVertex(depth, hot.h)) {
    ++g_st.six;
    return false;
  }
  ++g_st.skipped;
  g_skipped_this_frame = true;
  return true;
}

bool FarCellSkip(const void* rec) {
  if (!g_pass.ok || !GetConfig().far_tree_cells || g_disabled) return false;
  const float* m = Mem<const float*>(kCamMatrix);
  if (!m || !rec) return false;
  ++g_st.cells_seen;
  // The bucket chain's signature: bucket and slot pointers only (the
  // objects themselves are not touched: that is what this saves).
  uint32_t sig = 2166136261u, n = 0;
  void* const* first = At<void* const*>(rec, 8);
  for (void* const* b = first; b; b = static_cast<void* const*>(b[0])) {
    sig = (sig ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b))) * 16777619u;
    for (int k = 1; k <= 4; ++k) {
      if (!b[k]) continue;
      ++n;
      sig = (sig ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(b[k]))) * 16777619u;
    }
  }
  CellInfo& c = g_cells[rec];
  if (c.sig != sig || c.objs != n) {
    // New or changed: look at every object once, and write the trees down.
    ++g_st.cells_built;
    c = CellInfo{};
    c.sig = sig;
    c.objs = n;
    c.pure = n > 0;
    c.x0 = c.z0 = 1e30f;
    c.x1 = c.z1 = -1e30f;
    const float s = At<float>(g_pass.mgr, 0x5A8);
    for (void* const* b = first; b && c.pure; b = static_cast<void* const*>(b[0])) {
      for (int k = 1; k <= 4 && c.pure; ++k) {
        void* obj = b[k];
        if (!obj) continue;
        const void* type = nullptr;
        if (Mem<uintptr_t>(reinterpret_cast<uintptr_t>(obj)) != kTreeVtable || At<uint8_t>(obj, 8) != g_pass.layer ||
            !(type = TypeOf(g_pass.mgr, obj)) || At<const void*>(type, 0x1E0) != g_pass.tex) {
          c.pure = false;
          break;
        }
        bool added = false;
        const size_t i = g_reg.Find(g_pass.mgr, obj, type, &added);
        if (added) ++g_st.added;
        const float x = static_cast<float>(At<uint16_t>(obj, 0xC)) * s;
        const float z = static_cast<float>(At<uint16_t>(obj, 0x10)) * s;
        c.x0 = std::min(c.x0, x);
        c.x1 = std::max(c.x1, x);
        c.z0 = std::min(c.z0, z);
        c.z1 = std::max(c.z1, z);
        c.hmax = std::max(c.hmax, g_reg.nums[i * 12 + 6]);
      }
    }
  }
  if (!c.pure) return false;
  ++g_st.cells_pure;
  // Depth is linear in x and z (0x457450): its smallest value over the box
  // is at a corner.
  const float dx = m[2], dz = m[10], d0 = m[14];
  const float near_d = std::min(c.x0 * dx, c.x1 * dx) + std::min(c.z0 * dz, c.z1 * dz) + d0;
  const float margin = 4.0f;
  if (!(near_d > g_pass.far_d + margin)) return false;
  if (g_pass.six_a8 && (near_d - margin) * g_pass.six_ac < c.hmax * 1.01f) return false;
  ++g_st.cells_skipped;
  g_st.cell_trees_skipped += c.objs;
  return true;
}

// The builder set the billboard state (0x45bba0 returned): draw the far
// trees now. `base`: the builder's stack frame (its esp in the loop).
extern "C" void FarTreesStateDone(void* mgr, const uint8_t* base) {
  if (!g_pass.ok || mgr != g_pass.mgr || !g_reg.Count()) {
    g_frame_ok = false;
    return;
  }
  prof::Scope zone(prof::kFarTrees);
  const double t0 = Now();
  const uint8_t* lod = LodEntry(mgr);
  const void* w = At<const void*>(mgr, 0x18);
  const void* cam = w ? At<const void*>(w, 8) : nullptr;
  const float* m = Mem<const float*>(kCamMatrix);
  if (!cam || !m) {
    g_frame_ok = false;
    return;
  }
  FarTreeFrame& f = g_frame;
  f.right[0] = At<float>(base, 0x78);
  f.right[1] = At<float>(base, 0x70);
  f.view[0] = m[2];
  f.view[1] = m[10];
  f.view[2] = m[14];
  f.key_scale = At<float>(mgr, 0x5B8);
  f.key_unit = g_pass.unit;
  f.far_depth = g_pass.far_d;
  f.eps = g_pass.unit * 2;
  f.max_depth = static_cast<float>(At<int32_t>(lod, 0xC));
  f.focal = At<float>(cam, 0x198);
  f.min_size = Mem<float>(0x5557E0);
  f.six_ac = g_pass.six_ac;
  f.six_near = Mem<float>(0x555C14);
  f.six_a8 = g_pass.six_a8;
  f.six_84 = g_pass.six_84;
  f.diffuse = 0xFF000000u | (At<uint32_t>(base, 0x20) & 0xFFFFFF);
  if (GetConfig().far_tree_log >= 2) f.diffuse = 0xFFFF4040u;  // FarTreeLog=2: tinted, to tell them apart
  const uint8_t* vb = At<const uint8_t*>(mgr, 0x50);
  f.specular = vb ? At<uint32_t>(vb, 0x14) : 0;
  f.trees = g_reg.nums.data();
  f.count = g_reg.Count();
  f.dirty_from = std::min(g_reg.dirty_from, f.count);
  g_frame_ok = true;
  if (dd::DrawFarTrees(f)) {
    g_reg.dirty_from = f.count;
    ++g_st.draws;
  } else {
    ++g_st.draw_fail;
    g_disabled = true;
    patch::Log("far trees: the draw failed; FarTrees off for this run");
  }
  g_skipped_this_frame = false;
  g_st.draw_ms += (Now() - t0) * 1000;
}

// 0x45c4e3: no near billboard left; run the billboard part anyway when far
// trees are waiting.
extern "C" int FarTreesWanted(void* mgr) {
  const bool want = g_pass.ok && mgr == g_pass.mgr && g_reg.Count() && At<const void*>(mgr, 0x38);
  if (want) ++g_st.empty_runs;
  return want ? 1 : 0;
}

// ---- FarTreeLog: the game's own billboards against the numbers.

void FarTreesCheckDraw(const void* verts, unsigned nverts, const uint16_t* idx, unsigned nidx) {
  if (!GetConfig().far_tree_log || !g_frame_ok || !g_pass.ok) return;
  static unsigned tick = 0;
  if (++tick % 97 != 0) return;
  const void* mgr = g_pass.mgr;
  const uint8_t* v = static_cast<const uint8_t*>(verts);
  // The builder's list (mgr+0x3c, mgr+0x5a0 entries): what each tree would give.
  void* const* list = At<void* const*>(mgr, 0x3C);
  const int32_t n = At<int32_t>(mgr, 0x5A0);
  if (!list || n <= 0) return;
  struct Pred {
    float v[4][5];  // x y z u v
    uint32_t col;
    bool emit4;
  };
  std::vector<Pred> preds;
  preds.reserve(n);
  const uint8_t* lod = LodEntry(mgr);
  const float max_depth = static_cast<float>(At<int32_t>(lod, 0xC));
  const float focal = g_frame.focal;
  for (int32_t i = 0; i < n; ++i) {
    const void* obj = list[i];
    const void* type = TypeOf(mgr, obj);
    const TreeNumbers t = Numbers(mgr, obj, type);
    const float depth = static_cast<float>(At<uint16_t>(obj, 6)) * At<float>(mgr, 0x5BC);
    Pred p{};
    const bool culled = !(depth < max_depth) || !(t.h * focal / depth > g_frame.min_size);
    const bool six = SixVertex(depth, t.h);
    if (!culled && !six && depth > g_frame.far_depth - g_frame.eps) ++g_st.chk_overlap;
    p.emit4 = !culled && !six;
    if (!culled) ++g_st.chk_pred_emit;
    const float cs = g_frame.right[0], cc = g_frame.right[1];
    const float x0 = t.f[0] + t.f[5] * cs, z0 = t.f[1] - t.f[5] * cc;
    const float x1 = t.f[0] - t.f[4] * cs, z1 = t.f[1] + t.f[4] * cc;
    const float q[4][5] = {{x0, t.f[2], z0, t.f[8], t.f[11]},
                           {x1, t.f[2], z1, t.f[9], t.f[11]},
                           {x1, t.f[3], z1, t.f[9], t.f[10]},
                           {x0, t.f[3], z0, t.f[8], t.f[10]}};
    std::memcpy(p.v, q, sizeof(q));
    const bool solid = (At<uint8_t>(obj, 0x16) & 1) >= At<int32_t>(type, 0x1D8);
    p.col = (solid ? 0xFF000000u : static_cast<uint32_t>(At<uint8_t>(obj, 0x13)) << 24) |
            (g_frame.diffuse & 0xFFFFFF);
    preds.push_back(p);
  }
  ++g_st.chk_frames;
  // The draw's quads: 4-vertex billboards (b, b+1, b+2, b, b+2, b+3) and
  // bent ones (6 vertices, 12 indices: (b b+4 b+5) ...), whose outer four
  // vertices are the same quad (the bend only adds a lit centre line).
  for (unsigned i = 0; i + 6 <= nidx;) {
    const uint16_t b = idx[i];
    bool bent = false;
    if (idx[i + 1] == b + 1 && idx[i + 2] == b + 2 && idx[i + 3] == b && idx[i + 4] == b + 2 && idx[i + 5] == b + 3) {
      i += 6;
    } else if (i + 12 <= nidx && idx[i + 1] == b + 4 && idx[i + 2] == b + 5) {
      bent = true;
      i += 12;
    } else {
      ++i;
      continue;
    }
    if (b + (bent ? 5u : 3u) >= nverts) break;
    ++(bent ? g_st.chk_six : g_st.chk_quads);
    float a[4][5];
    uint32_t col[4];
    for (int k = 0; k < 4; ++k) {
      const uint8_t* p = v + (b + k) * 32u;
      std::memcpy(a[k], p, 12);
      std::memcpy(&a[k][3], p + 0x18, 8);
      std::memcpy(&col[k], p + 0x10, 4);
    }
    // The prediction with the nearest first vertex.
    const Pred* best = nullptr;
    float bd = 1e30f;
    for (const Pred& p : preds) {
      const float dx = p.v[0][0] - a[0][0], dz = p.v[0][2] - a[0][2];
      const float d = dx * dx + dz * dz;
      if (d < bd) bd = d, best = &p;
    }
    if (!best || bd > 1.0f) {
      ++g_st.chk_unmatched;
      if (g_st.chk_unmatched <= 3)
        patch::Log("far trees: check: %s billboard at (%.2f %.2f %.2f) matches no tree of the list (nearest %.2f away)",
                   bent ? "bent" : "flat", a[0][0], a[0][1], a[0][2], std::sqrt(bd));
      continue;
    }
    ++(bent ? g_st.chk_six_matched : g_st.chk_matched);
    if (!bent && !best->emit4) ++g_st.chk_pred_culled_but_drawn;
    float ep = 0, eu = 0;
    for (int k = 0; k < 4; ++k) {
      for (int c = 0; c < 3; ++c) ep = std::max(ep, std::fabs(best->v[k][c] - a[k][c]));
      for (int c = 3; c < 5; ++c) eu = std::max(eu, std::fabs(best->v[k][c] - a[k][c]));
      if (!bent && GetConfig().far_tree_log < 2 && col[k] != best->col) ++g_st.chk_col_bad;
    }
    g_st.chk_max_pos = std::max(g_st.chk_max_pos, ep);
    g_st.chk_max_uv = std::max(g_st.chk_max_uv, eu);
    if ((ep > 0.05f || eu > 0.001f || (!bent && GetConfig().far_tree_log < 2 && col[0] != best->col)) && ++g_st.chk_bad <= 3)
      patch::Log("far trees: check: %s billboard differs (pos %.3f, uv %.4f): game v0 (%.3f %.3f %.3f | %.4f %.4f) "
                 "v2 (%.3f %.3f %.3f | %.4f %.4f) colour %08x; ours v0 (%.3f %.3f %.3f | %.4f %.4f) "
                 "v2 (%.3f %.3f %.3f | %.4f %.4f) colour %08x",
                 bent ? "bent" : "flat", ep, eu, a[0][0], a[0][1], a[0][2], a[0][3], a[0][4], a[2][0], a[2][1],
                 a[2][2], a[2][3], a[2][4], col[0], best->v[0][0], best->v[0][1], best->v[0][2], best->v[0][3],
                 best->v[0][4], best->v[2][0], best->v[2][1], best->v[2][2], best->v[2][3], best->v[2][4], best->col);
  }
}

namespace {

void MaybeLog() {
  const double now = Now();
  if (!g_st_since) g_st_since = now;
  if (now - g_st_since < 5.0) return;
  if (GetConfig().far_tree_log && g_st.passes) {
    const double p = g_st.passes;
    patch::Log("far trees 5s: %u passes; %zu trees written down (+%u), drawn here %.0f/frame, left to the game: "
               "%.0f near, %.1f bent, %.1f other texture; lookups %.1f%% in order (%u out of order, %u picked up again); %u draws (%.3f ms each on the CPU), %u frames with no "
               "billboard state call, %u runs with only far trees, %u resets, %u failed",
               g_st.passes, g_reg.Count(), g_st.added, g_st.skipped / p, g_st.near_trees / p, g_st.six / p,
               g_st.other_tex / p,
               g_st.seq_hits + g_st.seq_misses ? 100.0 * g_st.seq_hits / (g_st.seq_hits + g_st.seq_misses) : 0.0,
               g_st.seq_misses, g_st.seq_resyncs, g_st.draws, g_st.draws ? g_st.draw_ms / g_st.draws : 0.0, g_st.missed,
               g_st.empty_runs, g_st.resets, g_st.draw_fail);
    if (GetConfig().far_tree_cells)
      patch::Log("far trees 5s cells: per pass %.0f cells looked at, %.0f only far-tree candidates, %.0f left out of "
                 "the walk (%.0f trees); %llu cells noted afresh, %zu known",
                 g_st.cells_seen / p, g_st.cells_pure / p, g_st.cells_skipped / p, g_st.cell_trees_skipped / p,
                 static_cast<unsigned long long>(g_st.cells_built), g_cells.size());
    if (g_st.chk_frames)
      patch::Log("far trees 5s check: %u frames, game billboards %u flat (%u matched) + %u bent (%u matched); "
                 "%u differ (max pos %.4f uv %.5f, %u vertex colours), %u unmatched; list trees the builder should draw %u, matched ones "
                 "we would have culled %u; near trees we would also draw (overlap) %u",
                 g_st.chk_frames, g_st.chk_quads, g_st.chk_matched, g_st.chk_six, g_st.chk_six_matched, g_st.chk_bad,
                 g_st.chk_max_pos, g_st.chk_max_uv, g_st.chk_col_bad, g_st.chk_unmatched, g_st.chk_pred_emit, g_st.chk_pred_culled_but_drawn,
                 g_st.chk_overlap);
  }
  g_st = Stats{};
  g_st_since = now;
}

}  // namespace

// Called by fixes_walk.cpp at the start of each vegetation pass, before
// FarTreesPassStart: counts frames whose far trees were left out but never
// drawn (the builder did not set its billboard state).
void FarTreesFrameEnd() {
  if (g_skipped_this_frame) ++g_st.missed;
  g_skipped_this_frame = false;
  MaybeLog();
}

#if defined(_MSC_VER)
__declspec(naked) void FarStateStub() {
  __asm {
    push ecx
    push dword ptr [esp+8]
    mov eax, 45BBA0h
    call eax
    pop ecx
    lea eax, [esp+8]
    push eax
    push ecx
    call FarTreesStateDone
    add esp, 8
    ret 4
  }
}
__declspec(naked) void FarCountStub() {
  __asm {
    cmp dword ptr [esi+5A0h], 0
    jne go
    push esi
    call FarTreesWanted
    add esp, 4
    test eax, eax
    jnz go
    mov eax, 45CB4Fh
    jmp eax
  go:
    mov eax, 45C4EFh
    jmp eax
  }
}
#else
extern "C" void FarStateStub();
extern "C" void FarCountStub();
asm(".text\n"
    ".globl _FarStateStub\n"
    "_FarStateStub:\n"
    "  pushl %ecx\n"
    "  pushl 8(%esp)\n"
    "  movl $0x45BBA0, %eax\n"
    "  call *%eax\n"
    "  popl %ecx\n"
    "  leal 8(%esp), %eax\n"
    "  pushl %eax\n"
    "  pushl %ecx\n"
    "  call _FarTreesStateDone\n"
    "  addl $8, %esp\n"
    "  ret $4\n"
    ".globl _FarCountStub\n"
    "_FarCountStub:\n"
    "  cmpl $0, 0x5A0(%esi)\n"
    "  jne 1f\n"
    "  pushl %esi\n"
    "  call _FarTreesWanted\n"
    "  addl $4, %esp\n"
    "  testl %eax, %eax\n"
    "  jnz 1f\n"
    "  movl $0x45CB4F, %eax\n"
    "  jmp *%eax\n"
    "1:\n"
    "  movl $0x45C4EF, %eax\n"
    "  jmp *%eax\n");
#endif

void ApplyFarTreeFixes() {
  QueryPerformanceFrequency(&g_freq);
  const Config& c = GetConfig();
  if (!c.far_trees) {
    patch::Log("far trees: FarTrees=0, the game builds every tree billboard");
    return;
  }
  if (!c.fast_object_walk) {
    patch::Log("far trees: needs FastObjectWalk=1; off");
    return;
  }
  const uint8_t count_orig[] = {0x39, 0x9E, 0xA0, 0x05, 0x00, 0x00, 0x0F, 0x84, 0x60, 0x06, 0x00, 0x00};
  const uint8_t call_orig[] = {0xE8, 0x7D, 0xF6, 0xFF, 0xFF};  // call 45BBA0h
  if (!patch::Matches(kCountCheck, count_orig, sizeof(count_orig)) ||
      !patch::Matches(kStateCall, call_orig, sizeof(call_orig))) {
    patch::Log("far trees: game code differs, FarTrees not applied");
    g_disabled = true;
    return;
  }
  patch::Jump(kCountCheck, reinterpret_cast<uintptr_t>(&FarCountStub), sizeof(count_orig));
  patch::Call(kStateCall, reinterpret_cast<const void*>(&FarStateStub), sizeof(call_orig));
  patch::Log("far trees: FarTrees=1 (FarTreeDistance %.0f, 0 = automatic; FarTreeCells=%d; FarTreeLog=%d)",
             c.far_tree_distance, c.far_tree_cells ? 1 : 0, c.far_tree_log);
}
