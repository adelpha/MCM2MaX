// FastObjectWalk: the scene's object passes over a long view distance.
//
// The game keeps its world objects (trees, fences, buildings, riders) in a
// grid (object at [0x690ce4]). Each frame the scene opens a query (0x532bd0
// from 0x5345f6: the grid cells in the camera's view, a linked list at
// grid+0x70) and walks the objects in those cells twice:
//
//  - the vegetation pass (0x45b890) through 0x4e1df0: per cell, every object
//    not seen yet this pass (word at obj+4 against the pass number at
//    grid+0x6c), at most 500 per cell; each gets its depth key (vfunc 0,
//    word at obj+6), the cell's objects are sorted by key and handed out
//    from the far end;
//  - the scene-object pass (0x509470) through 0x4e1d30: every object, and
//    those of the pass's layer (byte obj+8 == this+0x44, and obj+0x14c == 0)
//    are drawn (0x444d80).
//
// A cell holds a chain of buckets (bucket[0] = next, [1..4] = objects); the
// cell list node's [1] points at a record whose +8 is the first bucket. Both
// walks chase cells, buckets and every object one pointer at a time, and the
// scene-object pass visits every tree only to skip it. With ViewDistance=10
// those two walks were ~13% of the frame (StOo's profile, 2026-11-14).
//
// With FastObjectWalk=1 the first walk of a query (either pass) also writes
// down the objects it found, in order, with their layer byte; the other pass
// then reads that list: the scene-object pass touches only objects of its
// own layer, the vegetation pass skips the cell and bucket chains. Same
// objects, same order, same calls. The list is dropped when the query ends
// (0x4e1f60, replaced by the same code plus that) and at the end of each
// scene.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "config.h"
#include "fartrees.h"
#include "patch.h"
#include "prof.h"

namespace {

constexpr uintptr_t kGridPtr = 0x690CE4;
constexpr uintptr_t kVegIterCall1 = 0x45B975;  // call 4e1df0 (first object of the vegetation pass)
constexpr uintptr_t kVegIterCall2 = 0x45BB57;  // call 4e1df0 (next object)
constexpr uintptr_t kVegIter = 0x4E1DF0;
constexpr uintptr_t kEndQuery = 0x4E1F60;
constexpr uintptr_t kObjectPass = 0x509470;
constexpr uintptr_t kCurrentGrid = 0x68FCB8;  // the game notes the grid being walked here

using ThisVoid = int(__fastcall*)(void* self, void* edx);
using ThisTwo = int(__fastcall*)(void* self, void* edx, int a, int b);
const ThisVoid kListFree = reinterpret_cast<ThisVoid>(0x424FE0);    // grid+0x64 list: free the query's cells
const ThisVoid kRestart = reinterpret_cast<ThisVoid>(0x4E1F40);     // start a pass over the open query
const ThisTwo kDrawObject = reinterpret_cast<ThisTwo>(0x444D80);    // scene object draw (obj, 0, 0)
const ThisVoid kPassTail = reinterpret_cast<ThisVoid>(0x46A230);    // the pass's base-class call after the walk

template <typename T>
T& At(void* p, size_t off) {
  return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off);
}

struct Entry {
  void* obj;
  uint8_t layer;
};

// The objects of the open query, in the game's walking order (cells, then
// buckets, then slots; each object at its first cell).
struct WalkList {
  void* grid = nullptr;
  void* head = nullptr;
  unsigned gen = 0;    // g_gen when made
  unsigned scene = 0;  // g_scene when made
  bool valid = false;
  std::vector<Entry> ents;
  std::vector<uint32_t> cell_end;  // per cell: end index in ents
};
// Two, so that a list being handed out by the vegetation pass is never
// remade under it; g_list is the newest.
WalkList g_lists[2];
WalkList* g_list_p = &g_lists[0];
#define g_list (*g_list_p)

unsigned g_gen = 1;    // bumped when a query ends
unsigned g_scene = 1;  // bumped at the end of each scene

struct Stats {
  unsigned builds = 0, built_by_veg = 0, built_by_obj = 0, reused_veg = 0, reused_obj = 0, fallback = 0;
  uint64_t cells = 0, buckets = 0, objects = 0, obj_matched = 0, veg_objects = 0, veg_capped = 0;
  unsigned checks = 0, check_bad = 0;
  LONGLONG build_ticks = 0;
} g_st;
DWORD g_last_log = 0;
LARGE_INTEGER g_freq;
unsigned g_check_tick = 0;
bool g_log = false;

void MaybeLog() {
  if (!g_log) return;
  const DWORD now = GetTickCount();
  if (!g_last_log) g_last_log = now;
  if (now - g_last_log < 5000) return;
  const double secs = (now - g_last_log) / 1000.0;
  g_last_log = now;
  const double b = g_st.builds ? g_st.builds : 1;
  patch::Log("walk %.1fs: %u lists made (%u by the vegetation pass, %u by the object pass), %.2f ms each; per list "
             "%.0f cells, %.0f buckets, %.0f objects; reused by vegetation %u, by objects %u; object pass drew "
             "%.0f per list; vegetation handed out %.0f per list (%llu over the 500-per-cell limit); game's own walk "
             "%u times; checks %u, %u differed",
             secs, g_st.builds, g_st.built_by_veg, g_st.built_by_obj,
             g_st.builds ? static_cast<double>(g_st.build_ticks) * 1000.0 / g_freq.QuadPart / g_st.builds : 0.0,
             g_st.cells / b, g_st.buckets / b, g_st.objects / b, g_st.reused_veg, g_st.reused_obj,
             g_st.obj_matched / b, g_st.veg_objects / b, static_cast<unsigned long long>(g_st.veg_capped),
             g_st.fallback, g_st.checks, g_st.check_bad);
  g_st = Stats{};
}

// Walk the open query's cells the game's way, appending first-seen objects.
// `stamp`: write the pass number into each object (as the game's walk does)
// and skip those that have it; else (a check) use `seen` to skip repeats.
void Collect(void* grid, std::vector<Entry>& ents, std::vector<uint32_t>& cell_end, bool stamp,
             std::unordered_set<void*>* seen, unsigned* ncells, unsigned* nbuckets) {
  const uint16_t pass = At<uint16_t>(grid, 0x6C);
  for (void** cell = At<void**>(grid, 0x70); cell; cell = static_cast<void**>(cell[0])) {
    ++*ncells;
    void* rec = cell[1];
    for (void** bucket = rec ? At<void**>(rec, 8) : nullptr; bucket; bucket = static_cast<void**>(bucket[0])) {
      ++*nbuckets;
      for (int s = 1; s <= 4; ++s) {
        void* obj = bucket[s];
        if (!obj) continue;
        if (stamp) {
          uint16_t& w = At<uint16_t>(obj, 4);
          if (w == pass) continue;
          w = pass;
        } else {
          if (!seen->insert(obj).second) continue;
        }
        ents.push_back({obj, At<uint8_t>(obj, 8)});
      }
    }
    cell_end.push_back(static_cast<uint32_t>(ents.size()));
  }
}

bool ListValid(void* grid) {
  return g_list.valid && g_list.grid == grid && g_list.gen == g_gen && g_list.scene == g_scene &&
         g_list.head == At<void*>(grid, 0x70) && (At<uint8_t>(grid, 0x68) & 1);
}

// Made during a pass that just started (its pass number is fresh), so the
// stamps it writes are the ones the game's walk would write.
const WalkList* VegList();

void Build(void* grid, bool by_veg) {
  prof::Scope zone(prof::kObjectWalk);
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  if (VegList() == g_list_p) g_list_p = g_list_p == &g_lists[0] ? &g_lists[1] : &g_lists[0];
  g_list.ents.clear();
  g_list.cell_end.clear();
  unsigned nc = 0, nb = 0;
  Collect(grid, g_list.ents, g_list.cell_end, true, nullptr, &nc, &nb);
  g_list.grid = grid;
  g_list.head = At<void*>(grid, 0x70);
  g_list.gen = g_gen;
  g_list.scene = g_scene;
  g_list.valid = true;
  QueryPerformanceCounter(&t1);
  ++g_st.builds;
  ++(by_veg ? g_st.built_by_veg : g_st.built_by_obj);
  g_st.cells += nc;
  g_st.buckets += nb;
  g_st.objects += g_list.ents.size();
  g_st.build_ticks += t1.QuadPart - t0.QuadPart;
}

// WalkLog: now and then, walk the cells again (without touching the
// objects' pass numbers) and compare with the list being reused.
void MaybeCheck(void* grid) {
  if (!g_log || ++g_check_tick % 240 != 0) return;
  std::vector<Entry> ents;
  std::vector<uint32_t> ends;
  std::unordered_set<void*> seen;  // a linear search here was O(n^2): 20-180 ms per check
  seen.reserve(g_list.ents.size() * 2);
  unsigned nc = 0, nb = 0;
  Collect(grid, ents, ends, false, &seen, &nc, &nb);
  ++g_st.checks;
  bool same = ents.size() == g_list.ents.size() && ends == g_list.cell_end;
  for (size_t i = 0; same && i < ents.size(); ++i)
    same = ents[i].obj == g_list.ents[i].obj && ents[i].layer == g_list.ents[i].layer;
  if (!same) {
    ++g_st.check_bad;
    if (g_st.check_bad <= 5)
      patch::Log("walk: check: list of %u objects in %u cells differs from a fresh walk (%u objects in %u cells)",
                 static_cast<unsigned>(g_list.ents.size()), static_cast<unsigned>(g_list.cell_end.size()),
                 static_cast<unsigned>(ents.size()), static_cast<unsigned>(ends.size()));
  }
}

// After a whole pass the game's walk leaves its cursor like this.
void LeaveCursorAtEnd(void* grid) {
  At<void*>(grid, 0x74) = nullptr;
  At<uint32_t>(grid, 0x78) = 0;
  At<void*>(grid, 0x7C) = nullptr;
  At<uint32_t>(grid, 0x80) = 0;
}

// ---- vegetation pass: replaces 0x4e1df0 at its two calls

struct VegState {
  void* grid = nullptr;
  uint16_t pass = 0;
  bool active = false;    // a pass was started (grid, pass say which)
  bool finished = false;  // and has handed out everything
  const WalkList* list = nullptr;           // the list this pass hands out
  size_t cell = 0;                          // next cell to collect
  std::vector<std::pair<uint32_t, void*>> cur;  // this cell's objects (key << 16 | order, obj), sorted
  size_t left = 0;                          // handed out from the end
} g_veg;

const WalkList* VegList() { return g_veg.active && !g_veg.finished ? g_veg.list : nullptr; }

void* __fastcall VegNext(void* grid, void*) {
  if (!(At<uint8_t>(grid, 0x68) & 1)) return nullptr;
  *reinterpret_cast<void**>(kCurrentGrid) = grid;
  const uint16_t pass = At<uint16_t>(grid, 0x6C);
  if (!g_veg.active || g_veg.grid != grid || g_veg.pass != pass) {
    // A new pass (the vegetation pass restarted the query just before).
    g_veg.active = false;
    FarTreesFrameEnd();
    FarTreesPassStart();
    if (ListValid(grid)) {
      ++g_st.reused_veg;
      MaybeCheck(grid);
    } else {
      Build(grid, true);
    }
    g_veg.grid = grid;
    g_veg.pass = pass;
    g_veg.active = true;
    g_veg.finished = false;
    g_veg.list = g_list_p;
    g_veg.cell = 0;
    g_veg.left = 0;
  }
  if (g_veg.finished) return nullptr;
  const WalkList& list = *g_veg.list;
  for (;;) {
    if (g_veg.left) return g_veg.cur[--g_veg.left].second;
    if (g_veg.cell >= list.cell_end.size()) {
      LeaveCursorAtEnd(grid);
      g_veg.finished = true;  // nothing more until the next pass
      return nullptr;
    }
    const size_t from = g_veg.cell ? list.cell_end[g_veg.cell - 1] : 0;
    const size_t to = list.cell_end[g_veg.cell];
    ++g_veg.cell;
    size_t n = to - from;
    if (n > 500) {  // the game's per-cell limit (the rest are marked seen and left out)
      g_st.veg_capped += n - 500;
      n = 500;
    }
    g_veg.cur.clear();
    for (size_t i = 0; i < n; ++i) {
      void* obj = list.ents[from + i].obj;
      // vfunc 0: the object works out its depth key (obj+6).
      reinterpret_cast<ThisVoid>((*static_cast<void***>(obj))[0])(obj, nullptr);
      if (FarTreeSkip(obj)) continue;  // FarTrees: drawn on the GPU instead
      g_veg.cur.push_back({static_cast<uint32_t>(At<uint16_t>(obj, 6)) << 16 | static_cast<uint32_t>(i), obj});
    }
    // By key, objects with the same key in the order found (the game's sort
    // with FastSort=1: a stable sort; two objects: swapped only if out of order).
    std::sort(g_veg.cur.begin(), g_veg.cur.end(),
              [](const std::pair<uint32_t, void*>& a, const std::pair<uint32_t, void*>& b) { return a.first < b.first; });
    g_veg.left = g_veg.cur.size();
    g_st.veg_objects += g_veg.left;
  }
}

// ---- scene-object pass: replaces 0x509470

int __fastcall ObjectPass(void* self, void*) {
  void* grid = *reinterpret_cast<void**>(kGridPtr);
  kRestart(grid, nullptr);
  At<int32_t>(self, 0x3C) = 0;
  // The game's walk (0x4e1d30) gives nothing while grid+0x864 is set.
  if ((At<uint8_t>(grid, 0x68) & 1) && At<uint32_t>(grid, 0x864) == 0) {
    *reinterpret_cast<void**>(kCurrentGrid) = grid;
    if (ListValid(grid)) {
      ++g_st.reused_obj;
      MaybeCheck(grid);
    } else {
      Build(grid, false);
    }
    const uint8_t layer = At<uint8_t>(self, 0x44);
    // Index loop: drawing an object must not change the list, but stay safe
    // if it is remade meanwhile.
    for (size_t i = 0; i < g_list.ents.size(); ++i) {
      const Entry e = g_list.ents[i];
      if (e.layer != layer) continue;
      if (At<uint32_t>(e.obj, 0x14C) != 0) continue;
      At<uint32_t>(e.obj, 0x2D4) = 1;
      kDrawObject(e.obj, nullptr, 0, 0);
      ++At<int32_t>(self, 0x3C);
      ++g_st.obj_matched;
    }
    LeaveCursorAtEnd(grid);
  }
  MaybeLog();
  return kPassTail(self, nullptr);
}

// ---- end of a query: replaces 0x4e1f60 (same code, and drops the list)

void __fastcall EndQuery(void* grid, void*) {
  ++g_gen;
  g_list.valid = false;
  if (!(At<uint8_t>(grid, 0x68) & 1)) return;
  kListFree(At<void*>(grid, 0x64), nullptr);
  At<uint32_t>(grid, 0x70) = 0;
  At<uint32_t>(grid, 0x74) = 0;
  At<uint32_t>(grid, 0x78) = 0;
  At<uint32_t>(grid, 0x80) = 0;
  At<uint32_t>(grid, 0x68) &= ~1u;
}

bool CallTargets(uintptr_t at, uintptr_t target) {
  uint8_t code[5] = {0xE8};
  const int32_t rel = static_cast<int32_t>(target - (at + 5));
  std::memcpy(code + 1, &rel, 4);
  return patch::Matches(at, code, sizeof(code));
}

}  // namespace

void WalkSceneEnd() {
  ++g_scene;
  g_list.valid = false;
}

void ApplyWalkFixes() {
  const Config& c = GetConfig();
  QueryPerformanceFrequency(&g_freq);
  g_log = c.walk_log;
  if (!c.fast_object_walk) {
    patch::Log("walk: FastObjectWalk=0, the game's own object walks");
    return;
  }
  const uint8_t end_orig[] = {0x56, 0x8B, 0xF1, 0xF6, 0x46, 0x68, 0x01};          // push esi; mov esi,ecx; test byte [esi+68h],1
  const uint8_t pass_orig[] = {0x56, 0x8B, 0xF1, 0x8B, 0x0D, 0xE4, 0x0C, 0x69, 0x00};  // push esi; mov esi,ecx; mov ecx,[690CE4h]
  const uint8_t restart_orig[] = {0x8B, 0x41, 0x70, 0x89, 0x41, 0x74};             // 4e1f40: mov eax,[ecx+70h]; mov [ecx+74h],eax
  if (!patch::Matches(kEndQuery, end_orig, sizeof(end_orig)) ||
      !patch::Matches(kObjectPass, pass_orig, sizeof(pass_orig)) ||
      !patch::Matches(0x4E1F40, restart_orig, sizeof(restart_orig)) || !CallTargets(kVegIterCall1, kVegIter) ||
      !CallTargets(kVegIterCall2, kVegIter)) {
    patch::Log("walk: game code differs, FastObjectWalk not applied");
    return;
  }
  g_list.ents.reserve(1 << 16);
  patch::Jump(kEndQuery, reinterpret_cast<uintptr_t>(&EndQuery), sizeof(end_orig));
  patch::Jump(kObjectPass, reinterpret_cast<uintptr_t>(&ObjectPass), sizeof(pass_orig));
  patch::Call(kVegIterCall1, reinterpret_cast<const void*>(&VegNext), 5);
  patch::Call(kVegIterCall2, reinterpret_cast<const void*>(&VegNext), 5);
  patch::Log("walk: FastObjectWalk=1, vegetation and scene-object passes share one object list per query (WalkLog=%d)",
             g_log ? 1 : 0);
}
