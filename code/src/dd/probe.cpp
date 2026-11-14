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

void ProbeArm(float nx, float ny) {
  g_nx = nx;
  g_ny = ny;
  g_scenes = g_draws = g_hit_draws = g_terrain_draws = g_lines = 0;
  g_have_view = false;
  g_stage = 1;
  patch::Log("probe: armed at %.4f %.4f (normalised); next frame captures the terrain view, the one after logs draws",
             nx, ny);
}

int ProbeStage() { return g_stage; }

bool ProbeTerrainView(float m[16], float* nx, float* ny) {
  if (!g_have_view) return false;
  std::memcpy(m, g_view_proj, sizeof(g_view_proj));
  *nx = g_nx;
  *ny = g_ny;
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
    bool front = false;
    if (!Inside(px, py, s[0], s[1], s[2], &front)) continue;
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
    patch::Log("probe: done: %u draws (%u terrain), %u with a triangle over the point", g_draws, g_terrain_draws,
               g_hit_draws);
    g_stage = 0;
  }
}

}  // namespace dd
