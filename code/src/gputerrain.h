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
#include <cstddef>
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

// Indices of one chunk drawn with one texture (material -1: untextured).
struct Range {
  uint32_t first_index, index_count;
  int32_t material;
};

struct Chunk {
  uint32_t first_index, index_count;
  int32_t base_vertex;
  uint32_t vertex_count;
  float lo[3], hi[3];  // bounding box
  uint32_t first_range, range_count;
};

struct Mesh {
  std::vector<Vertex> verts;
  std::vector<uint16_t> idx;  // relative to the chunk's base_vertex
  std::vector<Chunk> chunks;
  std::vector<Range> ranges;
  std::vector<void*> textures;  // per material: the renderer's texture (a reference held), or null
  void* detail = nullptr;       // the detail texture (a reference held), or null
  float detail_fade[2] = {0, 0};  // its fade: from, to (depth)
  // The game's near/far split (0x481c00): blocks whose texel measure is at
  // or below T+0xca4 are drawn "far": no detail texture, diffuse T+0xc9c and
  // specular T+0xca0. far_k * T+0x70 (the camera's focal length in pixels)
  // is the distance where that happens; far_dist is this frame's (0 = off),
  // focal the focal length in pixels.
  float far_k = 0, far_dist = 0, focal = 0;
  uint32_t far_diffuse = 0xFFFFFFFFu, far_specular = 0;
  unsigned generation = 0;      // bumped for each new mesh
};

// Draw statistics the device returns for the log.
struct DrawStats {
  unsigned chunks_drawn = 0, tris_drawn = 0, draws = 0;
  double upload_ms = 0;
  bool uploaded = false;
};

}  // namespace gt

namespace dd {
// Draws the mesh with the current fog; false when it can't (Direct3D 9, no
// device). Uploads it first when its generation is new.
bool DrawGpuTerrain(const gt::Mesh& m, gt::DrawStats* stats);
bool GpuTerrainDrawable();
// The texture now set on stage 0 (a reference added), its release, and a
// description for the log.
void* GpuTerrainCurrentTexture();
void GpuTerrainReleaseTexture(void* t);
void GpuTerrainAddRefTexture(void* t);
void GpuTerrainDescribeTexture(void* t, char* out, size_t n);
// The game's own terrain pass state, captured where it would draw: `site`
// 0 = base (0x481870 sites, stage 0), 1 = one-pass (stage 0 base, stage 1
// detail), 2 = two-pass detail (stage 0). The GPU terrain samples its base
// and detail textures the way the game last set them up. With `log`, the
// state and the next few draws of that pass are written to the log.
void GpuTerrainCaptureState(int site, bool log);
}  // namespace dd

// fixes_terrain.cpp. `root`: the game's terrain root node.
// GpuTerrainPrepare (in the terrain update): builds the mesh when the track
// changed; true when GpuTerrain draws the terrain (the game's terrain phases
// are skipped). GpuTerrainDraw (where the game draws its terrain, 0x481870's
// call sites): draws it; false = the game draws its own.
bool GpuTerrainPrepare(void* root);
// GpuTerrainLog: logs where the game draws its terrain "near" and "far".
void GpuTerrainProbeGame(void* root, const float* eye);
bool GpuTerrainDraw(void* root);
bool GpuTerrainActive();
void GpuTerrainKeys();  // Ctrl+F7 switches between it and the game's terrain
