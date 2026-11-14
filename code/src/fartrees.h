// FarTrees: distant tree billboards drawn on the graphics card (fartrees.cpp).
#pragma once
#include <cstddef>
#include <cstdint>

// Per frame: what the game's billboard builder (0x45be20) would use.
struct FarTreeFrame {
  float right[2];      // camera direction (x, z), normalised: v0 = p + Rt * (right[0], -right[1])
  float view[3];       // depth = x * view[0] + z * view[1] + view[2] (the game's camera matrix _13 _33 _43)
  float key_scale;     // depth key = trunc(depth * key_scale), 0..65535
  float key_unit;      // and back: key * key_unit
  float far_depth;     // drawn here when the depth is past this (minus eps)
  float eps;           // overlap with the game's own trees (one key step and a bit)
  float max_depth;     // the game's billboard distance limit
  float focal;         // size cull: H * focal / depth > min_size
  float min_size;
  float six_ac;        // the game's bent (6-vertex) billboard: depth * six_ac < H
  float six_near;      //   or (six_84) depth < six_near
  int six_a8, six_84;  //   (only when six_a8)
  uint32_t diffuse, specular;
  const float* trees;  // 12 floats per tree (see fartrees.cpp)
  size_t count;
  size_t dirty_from;   // trees from here on are new since the last draw
};

namespace dd {
// Draws them now with the device's current state (the game's billboard
// state). False if it could not (no device, D3D9, shader failed).
bool DrawFarTrees(const FarTreeFrame& f);
// Whether far trees can be drawn at all (D3D11 and its shader).
bool FarTreesDrawable();
}  // namespace dd

void ApplyFarTreeFixes();
// FastObjectWalk's vegetation pass: called at the start of a pass, and for
// each object (after its depth key); true = a far tree, not handed to the game.
void FarTreesFrameEnd();
void FarTreesPassStart();
bool FarTreeSkip(void* obj);
// FarTreeCells: the grid cell record `rec` (the cell list node's [1]) holds
// only far trees this frame: the walk can leave the whole cell out.
bool FarCellSkip(const void* rec);
// From the device: the game's billboard draws (FVF 0x1e2 from the builder).
void FarTreesCheckDraw(const void* verts, unsigned nverts, const uint16_t* idx, unsigned nidx);
