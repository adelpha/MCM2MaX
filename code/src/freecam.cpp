// FreeCam=1: Ctrl+F6 during a race freezes the race and gives a fly camera
// (for screenshots from the same spot, e.g. comparing GpuTerrain on/off).
//
// The game's camera object keeps its eye at +0x170, the point it looks at at
// +0x17c, an up vector at +0x188 and a roll at +0x194; the camera update
// (0x42fd50, hooked in fixes_widescreen.cpp) builds the view (+0xac), the
// projection and the culling planes from those. While flying, the hook
// replaces the four with ours before the game builds anything, so the game
// culls and picks detail for where we are. The race simulation is not run
// at all while frozen (framerate.cpp skips the update calls), so the bikes,
// riders, timers and the game's own camera stop where they are; drawing
// carries on, and FreeCamFrame calls the camera update itself every frame.
//
// Controls: mouse or arrow keys look, W/A/S/D move (along the view), Space/E
// up, C/Q down, Shift faster, Z slower. Ctrl+F6 again puts the camera back
// and the race carries on from where it stopped.
#include "freecam.h"

#include <windows.h>

#include <cmath>

#include "config.h"
#include "dd/racetitle.h"
#include "patch.h"

namespace {

constexpr uintptr_t kCameraUpdate = 0x42FD50;
constexpr float kPi = 3.14159265f;

uintptr_t g_cam = 0;      // the main view's camera (last seen)
DWORD g_cam_stamp = 0;    // GetTickCount() when it was last updated
bool g_on = false;
bool g_f6 = false;
float g_eye[3] = {};
float g_yaw = 0.0f, g_pitch = 0.0f;  // radians; yaw 0 looks along +z, pitch up is positive
POINT g_center = {};
bool g_mouse = false;
unsigned g_frames = 0;
long g_mouse_total[2] = {};

float* F(uintptr_t cam, int off) { return reinterpret_cast<float*>(cam + off); }

bool Foreground() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void Forward(float out[3]) {
  out[0] = std::cos(g_pitch) * std::sin(g_yaw);
  out[1] = std::sin(g_pitch);
  out[2] = std::cos(g_pitch) * std::cos(g_yaw);
}

void CenterCursor() {
  RECT r;
  HWND w = GetForegroundWindow();
  if (w && GetClientRect(w, &r)) {
    POINT p = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    ClientToScreen(w, &p);
    g_center = p;
  } else {
    g_center = {GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2};
  }
  SetCursorPos(g_center.x, g_center.y);
}

void Start() {
  const uintptr_t c = g_cam;
  const float* eye = F(c, 0x170);
  const float* at = F(c, 0x17c);
  const float* up = F(c, 0x188);
  float d[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
  const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  for (int i = 0; i < 3; ++i) g_eye[i] = eye[i];
  if (len > 1e-4f) {
    g_yaw = std::atan2(d[0], d[2]);
    g_pitch = std::asin(d[1] / len);
  }
  g_on = true;
  g_frames = 0;
  g_mouse_total[0] = g_mouse_total[1] = 0;
  CenterCursor();
  g_mouse = true;
  patch::Log("freecam: ON (Ctrl+F6), race frozen. cam %08x eye (%.2f %.2f %.2f) at (%.2f %.2f %.2f) up (%.3f %.3f "
             "%.3f) roll %.4f fov %.2f -> yaw %.1f pitch %.1f deg",
             static_cast<unsigned>(c), eye[0], eye[1], eye[2], at[0], at[1], at[2], up[0], up[1], up[2],
             *F(c, 0x194), *F(c, 0x16c), g_yaw * 180.0f / kPi, g_pitch * 180.0f / kPi);
  dd::DebugNotice("Free camera (race frozen)");
}

void Stop(const char* why) {
  g_on = false;
  patch::Log("freecam: off (%s) after %u frames; eye (%.2f %.2f %.2f) yaw %.1f pitch %.1f; mouse moved %ld, %ld",
             why, g_frames, g_eye[0], g_eye[1], g_eye[2], g_yaw * 180.0f / kPi, g_pitch * 180.0f / kPi,
             g_mouse_total[0], g_mouse_total[1]);
  dd::DebugNotice("Free camera off");
}

}  // namespace

bool FreeCamActive() { return g_on; }

void __stdcall FreeCamApply(uintptr_t cam) {
  const int w = *reinterpret_cast<int*>(cam + 0x1a8);
  const int h = *reinterpret_cast<int*>(cam + 0x1ac);
  if (w < 320 || h <= 0 || w * 10 < h * 13) return;  // not the main view (e.g. the square shadow camera)
  if (!g_on) {
    g_cam = cam;
    g_cam_stamp = GetTickCount();
    return;
  }
  if (cam != g_cam) return;
  float f[3];
  Forward(f);
  float* eye = F(cam, 0x170);
  float* at = F(cam, 0x17c);
  float* up = F(cam, 0x188);
  for (int i = 0; i < 3; ++i) {
    eye[i] = g_eye[i];
    at[i] = g_eye[i] + f[i] * 100.0f;
  }
  up[0] = 0.0f;
  up[1] = 1.0f;
  up[2] = 0.0f;
  *F(cam, 0x194) = 0.0f;
}

bool FreeCamFrame(bool racing, double dt) {
  const Config& cfg = GetConfig();
  if (!cfg.free_cam) return false;
  const bool hit = Down(VK_F6) && !g_f6;
  g_f6 = Down(VK_F6);
  if (hit && Down(VK_CONTROL) && Foreground()) {
    if (g_on) {
      Stop("Ctrl+F6");
    } else if (!racing) {
      patch::Log("freecam: Ctrl+F6 ignored, no race running");
    } else if (!g_cam || GetTickCount() - g_cam_stamp > 2000) {
      patch::Log("freecam: Ctrl+F6 ignored, no race camera seen recently (cam %08x)", static_cast<unsigned>(g_cam));
    } else {
      Start();
    }
  }
  if (!g_on) return false;
  if (!racing) {
    Stop("race ended");
    return false;
  }
  ++g_frames;
  if (dt > 0.1) dt = 0.1;
  const float t = static_cast<float>(dt);

  if (Foreground()) {
    // Look: mouse (cursor moved from the centre, put back each frame) and arrow keys.
    POINT p;
    if (GetCursorPos(&p)) {
      if (!g_mouse) {
        CenterCursor();
        g_mouse = true;
      } else {
        const long dx = p.x - g_center.x, dy = p.y - g_center.y;
        if (dx || dy) {
          g_mouse_total[0] += dx;
          g_mouse_total[1] += dy;
          const float sens = cfg.free_cam_mouse * 0.0001f;  // radians per pixel
          g_yaw += dx * sens;
          g_pitch -= dy * sens;
          SetCursorPos(g_center.x, g_center.y);
        }
      }
    }
    const float turn = 1.5f * t;  // radians per second with the arrow keys
    if (Down(VK_LEFT)) g_yaw -= turn;
    if (Down(VK_RIGHT)) g_yaw += turn;
    if (Down(VK_UP)) g_pitch += turn;
    if (Down(VK_DOWN)) g_pitch -= turn;
    const float lim = 89.0f * kPi / 180.0f;
    if (g_pitch > lim) g_pitch = lim;
    if (g_pitch < -lim) g_pitch = -lim;

    // Move.
    float speed = cfg.free_cam_speed;
    if (Down(VK_SHIFT)) speed *= 5.0f;
    if (Down('Z')) speed *= 0.2f;
    float f[3];
    Forward(f);
    const float r[3] = {std::cos(g_yaw), 0.0f, -std::sin(g_yaw)};
    float m[3] = {};
    auto add = [&](const float* v, float s) {
      for (int i = 0; i < 3; ++i) m[i] += v[i] * s;
    };
    if (Down('W')) add(f, 1.0f);
    if (Down('S')) add(f, -1.0f);
    if (Down('D')) add(r, 1.0f);
    if (Down('A')) add(r, -1.0f);
    if (Down(VK_SPACE) || Down('E')) m[1] += 1.0f;
    if ((Down('C') || Down('Q')) && !Down(VK_CONTROL)) m[1] -= 1.0f;
    for (int i = 0; i < 3; ++i) g_eye[i] += m[i] * speed * t;
  } else {
    g_mouse = false;  // re-centre when the window is back
  }

  if (g_frames <= 3 || g_frames % 600 == 0)
    patch::Log("freecam: frame %u eye (%.2f %.2f %.2f) yaw %.1f pitch %.1f", g_frames, g_eye[0], g_eye[1], g_eye[2],
               g_yaw * 180.0f / kPi, g_pitch * 180.0f / kPi);

  // The race isn't simulated, so the game may not update its camera: do it.
  using ThisFn = void(__thiscall*)(void*);
  reinterpret_cast<ThisFn>(kCameraUpdate)(reinterpret_cast<void*>(g_cam));
  return true;
}
