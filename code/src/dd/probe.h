// Pixel probe (TerrainLog=1, Ctrl+F11): which draws cover one point of the
// screen, for finding why distant terrain goes missing.
#pragma once

#include <cstdint>

namespace dd {

// Arms the probe for the point at normalised device coordinates (nx, ny):
// the next frame captures the matrices terrain is drawn with, the frame
// after logs every draw with a triangle over the point.
void ProbeArm(float nx, float ny, bool log_point = true);
// 0 = idle, 1 = capturing matrices, 2 = logging draws and finding holes,
// 3 = (a hole was found) walking the terrain nodes at the biggest hole.
int ProbeStage();
// Whether this probe logs a point (Ctrl+F11) or only looks for holes.
bool ProbeLogsPoint();
// The view x projection matrix terrain was last drawn with (row vectors,
// D3D order) and the point; false if none captured yet.
bool ProbeTerrainView(float m[16], float* nx, float* ny);

// From the device: a draw (site = the game's call site or 0).
void ProbeDraw(uint32_t site, uint32_t prim, uint32_t fvf, bool tl, const uint8_t* verts, uint32_t stride,
               uint32_t count, const uint16_t* idx, uint32_t nidx, const float* world, const float* view,
               const float* proj, const float* wvp, float vp_x, float vp_y, float vp_w, float vp_h, bool zenable);
// From the device at EndScene.
void ProbeEndScene();

}  // namespace dd
