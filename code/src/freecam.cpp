// FreeCam=1: Ctrl+F6 during a race freezes the race and gives a fly camera
// (for screenshots from the same spot, e.g. comparing GpuTerrain on/off).
//
// The game's camera object keeps its eye at +0x170, its look direction at
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

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

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
unsigned g_frames = 0;
float g_side = 1.0f;  // -1 if the game's right is the other way (found from its view matrix)
long g_mouse_total[2] = {};

// Mouse: the game's DirectInput mouse keeps the Windows cursor from moving
// (gterr-6: no cursor movement at all), so read the mouse with our own
// DirectInput 8 device, non-exclusive, which gets movement alongside it.
IDirectInput8A* g_di = nullptr;
IDirectInputDevice8A* g_mdev = nullptr;
bool g_di_tried = false;

bool MouseInit() {
  if (g_mdev) return true;
  if (g_di_tried) return false;
  g_di_tried = true;
  HMODULE m = LoadLibraryA("dinput8.dll");
  using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
  auto create = m ? reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(m, "DirectInput8Create"))) : nullptr;
  if (!create) {
    patch::Log("freecam: dinput8.dll not available, no mouse look");
    return false;
  }
  static const GUID kIID8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
  static const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
  static const GUID kXAxis = {0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
  static const GUID kYAxis = {0xA36D02E1, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
  static DIOBJECTDATAFORMAT objs[] = {
      {&kXAxis, 0, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0},
      {&kYAxis, 4, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0},
  };
  static DIDATAFORMAT fmt = {sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS, 16, 2, objs};
  HRESULT hr = create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, kIID8A, reinterpret_cast<void**>(&g_di), nullptr);
  if (FAILED(hr) || !g_di) {
    patch::Log("freecam: DirectInput8Create failed hr=0x%08lx, no mouse look", static_cast<unsigned long>(hr));
    return false;
  }
  hr = g_di->CreateDevice(kSysMouse, &g_mdev, nullptr);
  if (SUCCEEDED(hr)) hr = g_mdev->SetDataFormat(&fmt);
  HWND w = GetForegroundWindow();
  if (SUCCEEDED(hr)) hr = g_mdev->SetCooperativeLevel(w, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
  if (FAILED(hr)) {
    patch::Log("freecam: mouse device setup failed hr=0x%08lx, no mouse look", static_cast<unsigned long>(hr));
    if (g_mdev) g_mdev->Release();
    g_mdev = nullptr;
    return false;
  }
  hr = g_mdev->Acquire();
  patch::Log("freecam: DirectInput 8 mouse ready (acquire hr=0x%08lx)", static_cast<unsigned long>(hr));
  return true;
}

// Mouse movement since the last call, in mickeys.
bool MouseDelta(long* dx, long* dy) {
  *dx = *dy = 0;
  if (!MouseInit()) return false;
  LONG st[4] = {};
  HRESULT hr = g_mdev->GetDeviceState(16, st);
  if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
    g_mdev->Acquire();
    hr = g_mdev->GetDeviceState(16, st);
  }
  if (FAILED(hr)) {
    static int said = 0;
    if (said++ < 5) patch::Log("freecam: mouse read failed hr=0x%08lx", static_cast<unsigned long>(hr));
    return false;
  }
  *dx = st[0];
  *dy = st[1];
  return true;
}

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

void Start() {
  const uintptr_t c = g_cam;
  const float* eye = F(c, 0x170);
  const float* at = F(c, 0x17c);
  const float* up = F(c, 0x188);
  // +0x17c is the look DIRECTION (a unit vector), not a point (gterr-6 log).
  float d[3] = {at[0], at[1], at[2]};
  const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  for (int i = 0; i < 3; ++i) g_eye[i] = eye[i];
  if (len > 1e-4f) {
    g_yaw = std::atan2(d[0], d[2]);
    g_pitch = std::asin(d[1] / len);
  }
  g_on = true;
  g_frames = 0;
  g_side = 1.0f;
  g_mouse_total[0] = g_mouse_total[1] = 0;
  long dx, dy;
  MouseDelta(&dx, &dy);  // drop movement from before
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
    at[i] = f[i];  // a direction
  }
  // Up, at right angles to the view (the game's is too).
  up[0] = -std::sin(g_pitch) * std::sin(g_yaw);
  up[1] = std::cos(g_pitch);
  up[2] = -std::sin(g_pitch) * std::cos(g_yaw);
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
    // Look: mouse (our own DirectInput device) and arrow keys.
    long dx, dy;
    if (MouseDelta(&dx, &dy) && (dx || dy)) {
      g_mouse_total[0] += dx;
      g_mouse_total[1] += dy;
      const float sens = cfg.free_cam_mouse * 0.0001f;  // radians per mickey
      g_yaw += g_side * dx * sens;
      g_pitch -= dy * sens;
    }
    const float turn = 1.5f * t;  // radians per second with the arrow keys
    if (Down(VK_LEFT)) g_yaw -= g_side * turn;
    if (Down(VK_RIGHT)) g_yaw += g_side * turn;
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
    const float r[3] = {g_side * std::cos(g_yaw), 0.0f, -g_side * std::sin(g_yaw)};
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
    long dx, dy;
    MouseDelta(&dx, &dy);  // don't turn from movement made elsewhere
  }

  if (g_frames <= 3 || g_frames % 600 == 0)
    patch::Log("freecam: frame %u eye (%.2f %.2f %.2f) yaw %.1f pitch %.1f", g_frames, g_eye[0], g_eye[1], g_eye[2],
               g_yaw * 180.0f / kPi, g_pitch * 180.0f / kPi);

  // The race isn't simulated, so the game may not update its camera: do it.
  using ThisFn = void(__thiscall*)(void*);
  reinterpret_cast<ThisFn>(kCameraUpdate)(reinterpret_cast<void*>(g_cam));
  if (g_frames == 1) {
    // Which way is "right" for the game: the view matrix (+0xac, row vectors)
    // has the camera's right axis in its first column.
    const float* v = F(g_cam, 0xac);
    const float gr[3] = {v[0], v[4], v[8]};
    const float mine[3] = {std::cos(g_yaw), 0.0f, -std::sin(g_yaw)};
    const float dot = gr[0] * mine[0] + gr[1] * mine[1] + gr[2] * mine[2];
    g_side = dot < 0.0f ? -1.0f : 1.0f;
    patch::Log("freecam: view right (%.3f %.3f %.3f) fwd (%.3f %.3f %.3f) vs ours (%.3f %.3f %.3f): side %+.0f", gr[0],
               gr[1], gr[2], v[2], v[6], v[10], mine[0], mine[1], mine[2], g_side);
  }
  return true;
}
