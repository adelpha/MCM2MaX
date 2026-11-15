// Pixel probe: see probe.h.
#include "probe.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "../patch.h"

namespace dd {
namespace {

volatile int g_stage = 0;
volatile float g_nx = 0, g_ny = 0;
float g_view_proj[16];
bool g_have_view = false;
unsigned g_scenes = 0, g_draws = 0, g_hit_draws = 0, g_terrain_draws = 0, g_lines = 0;

// Hole finder: terrain coverage of the frame at a quarter of the viewport's
// size. Terrain is a height field, so below the highest terrain pixel of a
// column every pixel should be terrain; one that is not is a hole.
constexpr int kCovW = 480, kCovH = 270;
unsigned char g_cov[kCovW * kCovH];
float g_hole_nx = 0, g_hole_ny = 0;
bool g_hole_found = false;
bool g_log_point = true;
unsigned g_auto_frames = 0;

void Cover(const float* a, const float* b, const float* c) {
  // NDC -> coverage pixels.
  float p[3][2];
  const float* v[3] = {a, b, c};
  for (int j = 0; j < 3; ++j) {
    p[j][0] = (v[j][0] + 1) * 0.5f * kCovW;
    p[j][1] = (1 - v[j][1]) * 0.5f * kCovH;
  }
  float x0 = std::fmin(p[0][0], std::fmin(p[1][0], p[2][0])), x1 = std::fmax(p[0][0], std::fmax(p[1][0], p[2][0]));
  float y0 = std::fmin(p[0][1], std::fmin(p[1][1], p[2][1])), y1 = std::fmax(p[0][1], std::fmax(p[1][1], p[2][1]));
  int ix0 = static_cast<int>(std::floor(x0)), ix1 = static_cast<int>(std::ceil(x1));
  int iy0 = static_cast<int>(std::floor(y0)), iy1 = static_cast<int>(std::ceil(y1));
  if (ix1 < 0 || iy1 < 0 || ix0 >= kCovW || iy0 >= kCovH) return;
  if (ix0 < 0) ix0 = 0;
  if (iy0 < 0) iy0 = 0;
  if (ix1 > kCovW - 1) ix1 = kCovW - 1;
  if (iy1 > kCovH - 1) iy1 = kCovH - 1;
  const float area = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1]);
  if (area == 0) return;
  for (int y = iy0; y <= iy1; ++y)
    for (int x = ix0; x <= ix1; ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      const float e0 = (p[1][0] - p[0][0]) * (py - p[0][1]) - (p[1][1] - p[0][1]) * (px - p[0][0]);
      const float e1 = (p[2][0] - p[1][0]) * (py - p[1][1]) - (p[2][1] - p[1][1]) * (px - p[1][0]);
      const float e2 = (p[0][0] - p[2][0]) * (py - p[2][1]) - (p[0][1] - p[2][1]) * (px - p[2][0]);
      if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) g_cov[y * kCovW + x] = 1;
    }
}

// After a frame's coverage: log the holes, keep the biggest run's middle.
void FindHoles() {
  unsigned holes = 0, runs = 0, best = 0;
  int bx = 0, by = 0;
  char buf[1024];
  int len = 0;
  buf[0] = 0;
  const int max_y = kCovH * 6 / 10;  // near the camera the near plane cuts terrain off
  for (int x = 0; x < kCovW; ++x) {
    int top = -1;
    for (int y = 0; y < kCovH; ++y)
      if (g_cov[y * kCovW + x]) {
        top = y;
        break;
      }
    if (top < 0) continue;
    for (int y = top + 1; y < max_y; ++y) {
      if (g_cov[y * kCovW + x]) continue;
      int e = y;
      while (e < max_y && !g_cov[e * kCovW + x]) ++e;
      const unsigned n = static_cast<unsigned>(e - y);
      holes += n;
      ++runs;
      if (n > best) best = n, bx = x, by = (y + e) / 2;
      if (len < 900) len += wsprintfA(buf + len, " x%d y%d-%d", x * 4, y * 4, e * 4);
      y = e;
    }
  }
  if (!holes) {
    if (g_log_point) patch::Log("probe: holes: none (terrain covers everything below its skyline)");
    g_hole_found = false;
    return;
  }
  g_hole_found = true;
  g_hole_nx = (bx + 0.5f) / kCovW * 2 - 1;
  g_hole_ny = 1 - (by + 0.5f) / kCovH * 2;
  patch::Log("probe: holes: %u quarter-pixels in %u column runs (full-size pixels):%s%s", holes, runs, buf,
             len >= 900 ? " ..." : "");
  patch::Log("probe: biggest hole at %d,%d (normalised %.4f %.4f); terrain nodes there follow next frame", bx * 4,
             by * 4, g_hole_nx, g_hole_ny);
}

void Mul(const float* a, const float* b, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = s;
    }
}

struct Clip {
  float x, y, z, w;
};
Clip Project(const float* p, const float* m) {
  return {p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12], p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13],
          p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14], p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15]};
}

// Is (px, py) inside the 2D triangle (either winding)?
bool Inside(float px, float py, const float* a, const float* b, const float* c, bool* front) {
  const float d1 = (px - b[0]) * (a[1] - b[1]) - (a[0] - b[0]) * (py - b[1]);
  const float d2 = (px - c[0]) * (b[1] - c[1]) - (b[0] - c[0]) * (py - c[1]);
  const float d3 = (px - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (py - a[1]);
  const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
  const float area = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
  *front = area < 0;
  return !(neg && pos);
}

}  // namespace

void ProbeArm(float nx, float ny, bool log_point) {
  g_log_point = log_point;
  g_nx = nx;
  g_ny = ny;
  g_scenes = g_draws = g_hit_draws = g_terrain_draws = g_lines = 0;
  g_have_view = false;
  std::memset(g_cov, 0, sizeof(g_cov));
  g_stage = 1;
  if (log_point)
    patch::Log("probe: armed at %.4f %.4f (normalised); next frame captures the terrain view, the one after logs draws",
               nx, ny);
}

int ProbeStage() { return g_stage; }
bool ProbeLogsPoint() { return g_log_point; }

bool ProbeTerrainView(float m[16], float* nx, float* ny) {
  if (!g_have_view) return false;
  std::memcpy(m, g_view_proj, sizeof(g_view_proj));
  *nx = g_stage == 3 ? g_hole_nx : g_nx;
  *ny = g_stage == 3 ? g_hole_ny : g_ny;
  return true;
}

void ProbeDraw(uint32_t site, uint32_t prim, uint32_t fvf, bool tl, const uint8_t* verts, uint32_t stride,
               uint32_t count, const uint16_t* idx, uint32_t nidx, const float* world, const float* view,
               const float* proj, const float* wvp, float vp_x, float vp_y, float vp_w, float vp_h, bool zenable) {
  if (!g_stage || !verts || !count || !stride) return;
  const bool terrain = site == 0x481D1E || site == 0x481DFE || site == 0x4820AF;
  if (terrain && g_stage == 1) {
    Mul(view, proj, g_view_proj);
    g_have_view = true;
    if (!g_terrain_draws++)
      patch::Log("probe: terrain world matrix row3 %.1f %.1f %.1f diag %.3f %.3f %.3f; viewport %.0f %.0f %.0fx%.0f",
                 world[12], world[13], world[14], world[0], world[5], world[10], vp_x, vp_y, vp_w, vp_h);
    return;
  }
  if (g_stage != 2) return;
  ++g_draws;
  if (terrain) ++g_terrain_draws;
  // Triangles of the draw.
  const uint32_t n = idx ? nidx : count;
  uint32_t tris = 0;
  if (prim == 4) tris = n / 3;            // list
  else if (prim == 5 || prim == 6) tris = n >= 3 ? n - 2 : 0;  // strip / fan
  else return;
  // The point in this draw's space: NDC for transformed draws, pixels for
  // pre-transformed ones.
  const float px = tl ? vp_x + (g_nx + 1) * 0.5f * vp_w : g_nx;
  const float py = tl ? vp_y + (1 - g_ny) * 0.5f * vp_h : g_ny;
  unsigned hits = 0;
  for (uint32_t t = 0; t < tris; ++t) {
    uint32_t k[3];
    if (prim == 4) k[0] = t * 3, k[1] = t * 3 + 1, k[2] = t * 3 + 2;
    else if (prim == 5) k[0] = t, k[1] = t + 1, k[2] = t + 2;
    else k[0] = 0, k[1] = t + 1, k[2] = t + 2;
    uint32_t vi[3];
    bool ok = true;
    for (int j = 0; j < 3; ++j) {
      vi[j] = idx ? idx[k[j]] : k[j];
      if (vi[j] >= count) ok = false;
    }
    if (!ok) continue;
    float s[3][3];
    float wpos[3][3];
    bool behind = false;
    for (int j = 0; j < 3; ++j) {
      const float* p = reinterpret_cast<const float*>(verts + static_cast<size_t>(vi[j]) * stride);
      std::memcpy(wpos[j], p, 12);
      if (tl) {
        s[j][0] = p[0], s[j][1] = p[1], s[j][2] = p[2];
      } else {
        const Clip c = Project(p, wvp);
        if (c.w <= 1e-4f) behind = true;
        const float iw = c.w > 1e-4f ? 1.0f / c.w : 0.0f;
        s[j][0] = c.x * iw, s[j][1] = c.y * iw, s[j][2] = c.z * iw;
      }
    }
    if (behind) continue;
    if (terrain) Cover(s[0], s[1], s[2]);
    bool front = false;
    if (!g_log_point || !Inside(px, py, s[0], s[1], s[2], &front)) continue;
    ++hits;
    if (g_lines < 300) {
      ++g_lines;
      patch::Log("probe: hit site %08x prim %u fvf %03x%s verts %u idx %u tri %u (%s) z %.6f %.6f %.6f zenable %d "
                 "pos (%.1f %.1f %.1f) (%.1f %.1f %.1f) (%.1f %.1f %.1f)",
                 site, prim, fvf, tl ? " TL" : "", count, nidx, t, front ? "front" : "BACK", s[0][2], s[1][2],
                 s[2][2], zenable ? 1 : 0, wpos[0][0], wpos[0][1], wpos[0][2], wpos[1][0], wpos[1][1], wpos[1][2],
                 wpos[2][0], wpos[2][1], wpos[2][2]);
    }
  }
  if (hits) ++g_hit_draws;
}

void ProbeEndScene() {
  ++g_scenes;
  if (g_stage == 1) {
    if (!g_have_view) return;  // no terrain drawn in this scene yet
    g_stage = 2;
    g_scenes = g_draws = g_terrain_draws = 0;
  } else if (g_stage == 2) {
    if (g_log_point)
      patch::Log("probe: done: %u draws (%u terrain), %u with a triangle over the point", g_draws, g_terrain_draws,
                 g_hit_draws);
    FindHoles();
    g_stage = g_hole_found ? 3 : 0;
  } else if (g_stage == 3) {
    g_stage = 0;
  }
}

}  // namespace dd
