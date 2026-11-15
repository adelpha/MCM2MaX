// GpuTerrain (ini, Direct3D 11): the whole track's terrain built once at
// full detail and kept on the GPU, drawn in a few chunks culled against the
// view, in place of the game's per-frame level-of-detail terrain (node
// selection, mesh building and thousands of small draws).
//
// Every height point of the game's terrain is used, exactly where the game
// puts it: the bike's collision (0x484cc0) reads the same full-resolution
// height grid, so what is drawn is what is ridden on. Physics is untouched.
//
// gputerrain.cpp walks the game's terrain tree (reading the parts not loaded
// yet from the track file through the game's own stream) and builds the
// mesh; dd/device.cpp (DrawGpuTerrain) uploads it and draws it.
#pragma once
#include <cstdint>
#include <vector>

namespace gt {

// The renderer's vertex (dd GpuVertex): position, diffuse, specular, uv x2.
struct Vertex {
  float pos[4];
  uint32_t diffuse, specular;
  float t0[2], t1[2];
};
static_assert(sizeof(Vertex) == 40, "gt::Vertex must match dd GpuVertex");

struct Chunk {
  uint32_t first_index, index_count;
  int32_t base_vertex;
  uint32_t vertex_count;
  float lo[3], hi[3];  // bounding box
};

struct Mesh {
  std::vector<Vertex> verts;
  std::vector<uint16_t> idx;  // relative to the chunk's base_vertex
  std::vector<Chunk> chunks;
  unsigned generation = 0;  // bumped for each new mesh
};

// Draw statistics the device returns for the log.
struct DrawStats {
  unsigned chunks_drawn = 0, tris_drawn = 0;
  double upload_ms = 0;
  bool uploaded = false;
};

}  // namespace gt

namespace dd {
// Draws the mesh with the current fog; false when it can't (Direct3D 9, no
// device). Uploads it first when its generation is new.
bool DrawGpuTerrain(const gt::Mesh& m, gt::DrawStats* stats);
bool GpuTerrainDrawable();
}  // namespace dd

// fixes_terrain.cpp. `root`: the game's terrain root node.
// GpuTerrainPrepare (in the terrain update): builds the mesh when the track
// changed; true when GpuTerrain draws the terrain (the game's terrain phases
// are skipped). GpuTerrainDraw (where the game draws its terrain, 0x481870's
// call sites): draws it; false = the game draws its own.
bool GpuTerrainPrepare(void* root);
bool GpuTerrainDraw(void* root);
bool GpuTerrainActive();
void GpuTerrainKeys();  // Ctrl+F7 switches between it and the game's terrain
