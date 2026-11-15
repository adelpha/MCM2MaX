// FastTerrain: exact rewrites of the game's per-frame terrain code
// (terrain_fast.cpp). Hooked from fixes_terrain.cpp.
#pragma once
#include <cstddef>
#include <cstdint>

namespace tf {

struct Node;

struct Options {
  bool lod_fix = false;          // TerrainLodFix clamp on the 0x48280f path
  bool hole_fix_hooked = false;  // TerrainHoleFix's stub is in (counting)
  bool hole_fix = false;         // ... and fixing
};
extern Options g_opt;

// Counters (game thread), read and cleared by the TerrainLog / FastTerrain
// report.
struct Stats {
  unsigned lod_calls = 0, lod_over = 0;
  unsigned hole_parents = 0, hole_fixed = 0;
  unsigned eval_nodes = 0, eval_tests = 0;
  unsigned draw_blocks = 0, draw_calls = 0;
  unsigned mesh_calls = 0, mesh_verts = 0;
  unsigned draw_verts = 0, draw_far = 0;
};
extern Stats g_stats;

// 0x482550 (this, flag): detail numbers; returns the near distance.
float Detail(Node* n, int flag);
// 0x4836e0 (this).
void Eval(Node* n);
// 0x4818b0 (this, a1, x, y, size, quadrant, a6); returns 1.
int DrawWalk(Node* n, int a1, int x, int y, int size, int q, int a6);

// 0x480c30 (this, level, triangles, x, y, dx, dy, out): a node's mesh
// triangles into the scratch builder 0x67da58; returns the triangle count.
int MeshTri(Node* n, int L, int cnt, int x, int y, int dx, int dy, int* out);
// 0x481640 (this unused; triangle, x, y, dx, dy): one corner triangle.
int MeshCorner(int t, int x, int y, int dx, int dy);

// The FPU is at 53-bit precision (the rewrites are exact only then).
bool FpuIs53();

}  // namespace tf
