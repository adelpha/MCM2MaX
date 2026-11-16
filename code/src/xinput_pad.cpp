// XInput controllers (Xbox pads) shown to the game as DirectInput joysticks.
//
// How the game uses joysticks (Mcm2.exe, found 2026-11-15):
//   0x4c3990 DirectInputCreateEx(version 0x700, IID_IDirectInput7A), then
//     EnumDevices(DIDEVTYPE_JOYSTICK, 0x4c3b10, ..., DIEDFL_ATTACHEDONLY): up to 8.
//   0x4c7080 per joystick: CreateDeviceEx(IID_IDirectInputDevice7A),
//     GetDeviceInfo, SetDataFormat(c_dfDIJoystick), SetCooperativeLevel
//     (EXCLUSIVE|BACKGROUND), GetCapabilities (DIDC_FORCEFEEDBACK -> force
//     feedback on), EnumObjects(axes) + GetObjectInfo (X/Y/Z/Rx/Ry/Rz by
//     guidType), GetProperty(DIPROP_RANGE) per axis, then BufferedJoystick
//     (ini, default 1): SetProperty(DIPROP_BUFFERSIZE) + Acquire.
//   0x4c80b0 each step: Poll, then GetDeviceData (buffered, 0x4c7850) or
//     GetDeviceState(DIJOYSTATE, 0x4c7ee0). Six axes, 4 POVs, 32 buttons.
//     The game uses Z as (range max - Z): a throttle slider. Joysticks whose
//     DIDEVTYPEJOYSTICK subtype is GAMEPAD get their axes turned into digital
//     repeats (0x4c74e0), so XInput pads, which Windows reports as gamepads,
//     steer digitally without this.
//   Force feedback: CreateEffect GUID_ConstantForce (0x4c8390, 0x4c8590) and
//     GUID_Square (0x4c86c0), SetParameters/Start/Stop/GetEffectStatus,
//     SetProperty(DIPROP_AUTOCENTER), SendForceFeedbackCommand, EnumEffects.
//
// With XInput=1 the game's IDirectInput7A is wrapped: EnumDevices lists each
// connected XInput pad first, as a TRADITIONAL joystick (analog axes), and
// leaves out the DirectInput copies Windows makes of the same pads (their HID
// path has "IG_"). The pads are DirectInput devices implemented here:
//   X, Y   left stick          Rx, Ry  right stick
//   Z      right trigger (sent inverted, so after the game's own inversion it
//          reads 0 at rest)    Rz      left trigger
//          The game's Accelerate uses the whole Z axis. Remapping an action
//          by pressing a trigger finds its button copy (XInputTriggerButtons),
//          not the axis: the game looks for movement from the centre.
//   POV 1  d-pad
//   buttons 1-10 A B X Y LB RB Back Start LS RS (XInputTriggerButtons: 11 LT, 12 RT)
// Force feedback effects become rumble. A never-ending constant force is the
// game pushing a force feedback stick; only its sudden jumps rumble unless
// XInputRumbleForces. Other constant and ramp forces and slow
// periodic ones drive the left (heavy) motor, faster periodic ones the right.
// A thread samples the pads every 4 ms, so buffered events carry their own
// time stamps (InputTiming hands them to the race step they belong to).

#define DIRECTINPUT_VERSION 0x0700
#include "xinput_pad.h"

#include <windows.h>
#include <dinput.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "config.h"
#include "patch.h"

namespace {

// ---------------------------------------------------------------- GUIDs
constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID kIID_DIA = {0x89521360, 0xAA8A, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kIID_DI2A = {0x5944E662, 0xAA8A, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kIID_DI7A = {0x9A4CB684, 0x236D, 0x11D3, {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
constexpr GUID kIID_DevA = {0x5944E680, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kIID_Dev2A = {0x5944E682, 0xC92E, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
constexpr GUID kIID_Dev7A = {0x57D7C6BC, 0x2356, 0x11D3, {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
constexpr GUID kIID_Effect = {0xE7E1F7C0, 0x88D2, 0x11D0, {0x9A, 0xD0, 0x00, 0xA0, 0xC9, 0xA0, 0x6E, 0x35}};

#define DI_OBJ_GUID(d1) {d1, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}}
constexpr GUID kGUID_XAxis = DI_OBJ_GUID(0xA36D02E0);
constexpr GUID kGUID_YAxis = DI_OBJ_GUID(0xA36D02E1);
constexpr GUID kGUID_ZAxis = DI_OBJ_GUID(0xA36D02E2);
constexpr GUID kGUID_RxAxis = DI_OBJ_GUID(0xA36D02F4);
constexpr GUID kGUID_RyAxis = DI_OBJ_GUID(0xA36D02F5);
constexpr GUID kGUID_RzAxis = DI_OBJ_GUID(0xA36D02E3);
constexpr GUID kGUID_Button = DI_OBJ_GUID(0xA36D02F0);
constexpr GUID kGUID_POV = DI_OBJ_GUID(0xA36D02F2);

#define DI_FF_GUID(d1) {d1, 0x8E33, 0x11D0, {0x9A, 0xD0, 0x00, 0xA0, 0xC9, 0xA0, 0x6E, 0x35}}
constexpr GUID kGUID_ConstantForce = DI_FF_GUID(0x13541C20);
constexpr GUID kGUID_RampForce = DI_FF_GUID(0x13541C21);
constexpr GUID kGUID_Square = DI_FF_GUID(0x13541C22);
constexpr GUID kGUID_Sine = DI_FF_GUID(0x13541C23);
constexpr GUID kGUID_Triangle = DI_FF_GUID(0x13541C24);
constexpr GUID kGUID_SawtoothUp = DI_FF_GUID(0x13541C25);
constexpr GUID kGUID_SawtoothDown = DI_FF_GUID(0x13541C26);

// Our pads: instance {78706164-6D63-6D32-8000-0000000000NN}, product below.
GUID PadInstanceGuid(int slot) {
  GUID g = {0x78706164, 0x6D63, 0x6D32, {0x80, 0, 0, 0, 0, 0, 0, static_cast<BYTE>(0x01 + slot)}};
  return g;
}
constexpr GUID kPadProduct = {0x78706164, 0x6D63, 0x6D32, {0x81, 0, 'X', 'I', 'N', 'P', 'U', 'T'}};

bool Same(REFGUID a, REFGUID b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

std::string GuidText(REFGUID g) {
  char b[64];
  std::snprintf(b, sizeof b, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", static_cast<unsigned long>(g.Data1),
                g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6],
                g.Data4[7]);
  return b;
}

// ---------------------------------------------------------------- XInput
using XGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
XGetStateFn g_get_state = nullptr;
XSetStateFn g_set_state = nullptr;
const char* g_xinput_dll = "";
// XInputGetCapabilitiesEx (xinput1_4 ordinal 108, Windows 10+): adds the
// controller's USB vendor and product id, used to name each slot.
struct XCapsEx {
  XINPUT_CAPABILITIES caps;
  WORD vid, pid, version, unk1;
  DWORD unk2;
};
using XCapsExFn = DWORD(WINAPI*)(DWORD, DWORD, DWORD, XCapsEx*);
using XCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
XCapsExFn g_caps_ex = nullptr;
XCapsFn g_caps = nullptr;
std::string g_slot_name[XUSER_MAX_COUNT];  // shown in the game's controller list

bool LoadXInput() {
  static bool tried = false;
  if (tried) return g_get_state != nullptr;
  tried = true;
  const char* names[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
  for (const char* n : names) {
    HMODULE m = LoadLibraryA(n);
    if (!m) continue;
    auto get = reinterpret_cast<XGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
    auto set = reinterpret_cast<XSetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputSetState")));
    if (get) {
      g_get_state = get;
      g_set_state = set;
      g_xinput_dll = n;
      g_caps_ex = reinterpret_cast<XCapsExFn>(reinterpret_cast<void*>(GetProcAddress(m, MAKEINTRESOURCEA(108))));
      g_caps = reinterpret_cast<XCapsFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetCapabilities")));
      patch::Log("xinput: using %s (rumble %s)", n, set ? "available" : "not available");
      return true;
    }
    FreeLibrary(m);
  }
  patch::Log("xinput: no XInput DLL found (xinput1_4/1_3/9_1_0); Xbox controllers stay as Windows' DirectInput devices");
  return false;
}

// ---------------------------------------------------------------- state
CRITICAL_SECTION g_cs;
struct Lock {
  Lock() { EnterCriticalSection(&g_cs); }
  ~Lock() { LeaveCriticalSection(&g_cs); }
};

bool g_log = false;
int g_rumble_pct = 100;
float g_deadzone = 0.15f;
float g_curve = 1.0f;          // XInputSteerCurve / 100: left stick X response exponent
bool g_rumble_forces = false;  // XInputRumbleForces: steady stick forces rumble too
double g_kick_threshold = 1000;  // XInputRumbleThreshold * 100: force jump that rumbles
float g_steer_smooth_us = 0;   // XInputSteerSmooth (ms) * 1000: steering follows the stick with this time constant
LARGE_INTEGER g_freq;

double NowUs() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return static_cast<double>(t.QuadPart) * 1e6 / static_cast<double>(g_freq.QuadPart);
}

// Objects of a pad, in DirectInput instance order.
constexpr int kAxes = 6, kPovs = 1, kButtons = 12;
constexpr int kObjPov = kAxes, kObjButton0 = kAxes + kPovs, kObjects = kAxes + kPovs + kButtons;

struct ObjDesc {
  const GUID* guid;
  DWORD type;  // DIDFT_ABSAXIS / DIDFT_POV / DIDFT_PSHBUTTON
  DWORD instance;
  DWORD default_ofs;  // in DIJOYSTATE
  const char* name;
};

const ObjDesc kObj[kObjects] = {
    {&kGUID_XAxis, DIDFT_ABSAXIS, 0, 0, "X Axis (left stick)"},
    {&kGUID_YAxis, DIDFT_ABSAXIS, 1, 4, "Y Axis (left stick)"},
    {&kGUID_ZAxis, DIDFT_ABSAXIS, 2, 8, "Z Axis (right trigger)"},
    {&kGUID_RxAxis, DIDFT_ABSAXIS, 3, 12, "X Rotation (right stick)"},
    {&kGUID_RyAxis, DIDFT_ABSAXIS, 4, 16, "Y Rotation (right stick)"},
    {&kGUID_RzAxis, DIDFT_ABSAXIS, 5, 20, "Z Rotation (left trigger)"},
    {&kGUID_POV, DIDFT_POV, 0, 32, "D-pad"},
    {&kGUID_Button, DIDFT_PSHBUTTON, 0, 48, "Button 1 (A)"},
    {&kGUID_Button, DIDFT_PSHBUTTON, 1, 49, "Button 2 (B)"},
    {&kGUID_Button, DIDFT_PSHBUTTON, 2, 50, "Button 3 (X)"},
    {&kGUID_Button, DIDFT_PSHBUTTON, 3, 51, "Button 4 (Y)"},
    {&kGUID_Button, DIDFT_PSHBUTTON, 4, 52, "Button 5 (LB)"},
    {&kGUID_Button, DIDFT_PSHBUTTON, 5, 53, "Button 6 (RB)"},
    {&kGUID_Button, DIDFT_PSHBUTTON, 6, 54, "Button 7 (Back)"},
    {&kGUID_Button, DIDFT_PSHBUTTON, 7, 55, "Button 8 (Start)"},
    {&kGUID_Button, DIDFT_PSHBUTTON, 8, 56, "Button 9 (left stick)"},
    {&kGUID_Button, DIDFT_PSHBUTTON, 9, 57, "Button 10 (right stick)"},
    {&kGUID_Button, DIDFT_PSHBUTTON, 10, 58, "Button 11 (LT)"},
    {&kGUID_Button, DIDFT_PSHBUTTON, 11, 59, "Button 12 (RT)"},
};

bool RumbleOn() { return g_set_state && g_rumble_pct > 0; }

bool g_trigger_buttons = true;   // XInputTriggerButtons: LT/RT also as buttons 11/12
bool ObjPresent(int i) { return g_trigger_buttons || i < kObjButton0 + 10; }
int ButtonCount() { return g_trigger_buttons ? kButtons : kButtons - 2; }

DWORD ObjType(int i) {
  DWORD t = kObj[i].type | DIDFT_MAKEINSTANCE(kObj[i].instance);
  if (i < 2 && RumbleOn()) t |= DIDFT_FFACTUATOR;
  return t;
}

class Pad;
class Effect;
std::vector<Pad*> g_pads;  // live device objects (under g_cs)
HANDLE g_thread = nullptr;
DWORD g_connected_mask = 0;  // last known XInput connection per slot (for logging)

const char* PropName(uintptr_t p) {
  switch (p) {
    case 1: return "BUFFERSIZE";
    case 2: return "AXISMODE";
    case 3: return "GRANULARITY";
    case 4: return "RANGE";
    case 5: return "DEADZONE";
    case 6: return "SATURATION";
    case 7: return "FFGAIN";
    case 8: return "FFLOAD";
    case 9: return "AUTOCENTER";
    case 10: return "CALIBRATIONMODE";
    default: return "other";
  }
}

// ---------------------------------------------------------------- effects
class Effect final : public IDirectInputEffect {
 public:
  Effect(Pad* pad, REFGUID type);
  ~Effect();

  STDMETHOD(QueryInterface)(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (Same(riid, kIID_IUnknown) || Same(riid, kIID_Effect)) {
      *out = static_cast<IDirectInputEffect*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHOD_(ULONG, AddRef)() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
  STDMETHOD_(ULONG, Release)() override {
    LONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
  }
  STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
  STDMETHOD(GetEffectGuid)(LPGUID g) override {
    if (!g) return E_POINTER;
    *g = type_;
    return DI_OK;
  }
  STDMETHOD(GetParameters)(LPDIEFFECT eff, DWORD flags) override;
  STDMETHOD(SetParameters)(LPCDIEFFECT eff, DWORD flags) override;
  STDMETHOD(Start)(DWORD iterations, DWORD flags) override;
  STDMETHOD(Stop)() override;
  STDMETHOD(GetEffectStatus)(LPDWORD status) override;
  STDMETHOD(Download)() override { return DI_OK; }
  STDMETHOD(Unload)() override { return Stop(); }
  STDMETHOD(Escape)(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }

  // Current strength 0..1 for the left (heavy) or right motor; ends the
  // effect when its time is up. Called under g_cs.
  void Strength(double now_us, float* low, float* high);
  // Force jumps at least this big (of 10000) rumble; the kick fades out
  // over kKickUs.
  // A crash stops the force outright (the game's crash state); that rumbles
  // at least kCrashKick for kCrashUs. Short jolts (70 ms in the game) are too
  // brief for pad motors to spin up, so they last at least kMinPulseUs.
  static constexpr double kKickUs = 400000, kCrashKick = 8000, kCrashUs = 700000,
                          kMinPulseUs = 160000;
  double kick_len_us_ = kKickUs;
  double KickLevel(double now_us) const {
    const double t = now_us - kick_us_;
    return t >= kick_len_us_ || t < 0 ? 0 : kick_ * (1.0 - t / kick_len_us_);
  }
  void Kick(double now_us, double level, double len_us) {
    const double left = KickLevel(now_us);
    if (level >= left) {
      kick_ = std::min(10000.0, level);
      kick_us_ = now_us;
      kick_len_us_ = len_us;
    }
  }

  Pad* pad_;
  GUID type_;
  bool periodic_ = false, ramp_ = false;
  bool playing_ = false;
  double start_us_ = 0;
  DWORD iterations_ = 1;
  DWORD duration_ = INFINITE, gain_ = 10000, start_delay_ = 0;
  DWORD axes_ = 0;
  bool has_env_ = false;
  DIENVELOPE env_ = {};
  LONG magnitude_ = 0, ramp_end_ = 0, offset_ = 0;  // constant/ramp start/periodic magnitude
  DWORD period_ = 0;
  // A constant force that never ends is the game pushing a force feedback
  // stick (steering weight, updated every frame). Unless XInputRumbleForces,
  // only its sudden changes (landings, crashes) rumble: kick_ decays from the
  // time it was set.
  bool steady_ = false;
  double kick_ = 0, kick_us_ = 0;

 private:
  volatile LONG refs_ = 1;
};

// ---------------------------------------------------------------- device
class Pad final : public IDirectInputDevice7A {
 public:
  explicit Pad(int slot) : slot_(slot) {
    for (int i = 0; i < kAxes; ++i) {
      min_[i] = 0;
      max_[i] = 65535;
      dz_[i] = 0;
      sat_[i] = 10000;
    }
    std::fill(std::begin(ofs_), std::end(ofs_), -1);
    std::fill(std::begin(last_), std::end(last_), 0);
    Lock lock;
    g_pads.push_back(this);
  }
  ~Pad() {
    Lock lock;
    SetMotors(0, 0);
    g_pads.erase(std::remove(g_pads.begin(), g_pads.end(), this), g_pads.end());
  }

  // IUnknown
  STDMETHOD(QueryInterface)(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (Same(riid, kIID_IUnknown) || Same(riid, kIID_DevA) || Same(riid, kIID_Dev2A) || Same(riid, kIID_Dev7A)) {
      *out = static_cast<IDirectInputDevice7A*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    if (g_log) patch::Log("xinput: pad %d: QueryInterface %s not supported", slot_ + 1, GuidText(riid).c_str());
    return E_NOINTERFACE;
  }
  STDMETHOD_(ULONG, AddRef)() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
  STDMETHOD_(ULONG, Release)() override {
    LONG r = InterlockedDecrement(&refs_);
    if (r == 0) {
      patch::Log("xinput: pad %d released", slot_ + 1);
      delete this;
    }
    return static_cast<ULONG>(r);
  }

  // IDirectInputDevice
  STDMETHOD(GetCapabilities)(LPDIDEVCAPS caps) override {
    if (!caps || caps->dwSize < sizeof(DIDEVCAPS_DX3)) return DIERR_INVALIDPARAM;
    const DWORD size = caps->dwSize;
    DIDEVCAPS c = {};
    c.dwSize = size;
    c.dwFlags = DIDC_ATTACHED | DIDC_POLLEDDEVICE | DIDC_POLLEDDATAFORMAT;
    if (RumbleOn()) c.dwFlags |= DIDC_FORCEFEEDBACK | DIDC_FFATTACK | DIDC_FFFADE;
    c.dwDevType = DevType();
    c.dwAxes = kAxes;
    c.dwButtons = static_cast<DWORD>(ButtonCount());
    c.dwPOVs = kPovs;
    if (RumbleOn()) {
      c.dwFFSamplePeriod = 4000;
      c.dwFFMinTimeResolution = 4000;
    }
    std::memcpy(caps, &c, std::min<DWORD>(size, sizeof c));
    caps->dwSize = size;
    if (g_log) patch::Log("xinput: pad %d: GetCapabilities (flags 0x%lx)", slot_ + 1, c.dwFlags);
    return DI_OK;
  }

  STDMETHOD(EnumObjects)(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) override {
    if (!cb) return DIERR_INVALIDPARAM;
    const DWORD want = DIDFT_GETTYPE(flags);
    int n = 0;
    for (int i = 0; i < kObjects; ++i) {
      if (!ObjPresent(i)) continue;
      if (want && !(kObj[i].type & want)) continue;
      if ((flags & DIDFT_FFACTUATOR) && !(ObjType(i) & DIDFT_FFACTUATOR)) continue;
      DIDEVICEOBJECTINSTANCEA o;
      FillObject(i, &o);
      ++n;
      if (cb(&o, ref) == DIENUM_STOP) break;
    }
    if (g_log) patch::Log("xinput: pad %d: EnumObjects(flags 0x%lx): %d objects", slot_ + 1, flags, n);
    return DI_OK;
  }

  STDMETHOD(GetProperty)(REFGUID prop, LPDIPROPHEADER h) override {
    if (!h) return DIERR_INVALIDPARAM;
    const uintptr_t p = reinterpret_cast<uintptr_t>(&prop);
    Lock lock;
    const int obj = FindObject(h->dwObj, h->dwHow);
    auto dword = [&](DWORD v) {
      if (h->dwSize < sizeof(DIPROPDWORD)) return DIERR_INVALIDPARAM;
      reinterpret_cast<DIPROPDWORD*>(h)->dwData = v;
      return DI_OK;
    };
    HRESULT hr = DIERR_UNSUPPORTED;
    switch (p) {
      case 1: hr = dword(buffer_size_); break;                              // BUFFERSIZE
      case 2: hr = dword(DIPROPAXISMODE_ABS); break;                        // AXISMODE
      case 3: hr = obj >= 0 ? dword(1) : DIERR_INVALIDPARAM; break;         // GRANULARITY
      case 4:                                                               // RANGE
        if (obj < 0 || obj >= kAxes || h->dwSize < sizeof(DIPROPRANGE)) {
          hr = DIERR_INVALIDPARAM;
        } else {
          reinterpret_cast<DIPROPRANGE*>(h)->lMin = min_[obj];
          reinterpret_cast<DIPROPRANGE*>(h)->lMax = max_[obj];
          hr = DI_OK;
        }
        break;
      case 5: hr = obj >= 0 && obj < kAxes ? dword(dz_[obj]) : DIERR_INVALIDPARAM; break;   // DEADZONE
      case 6: hr = obj >= 0 && obj < kAxes ? dword(sat_[obj]) : DIERR_INVALIDPARAM; break;  // SATURATION
      case 7: hr = dword(ff_gain_); break;                                                   // FFGAIN
      case 9: hr = dword(autocenter_); break;                                                // AUTOCENTER
      case 10: hr = dword(DIPROPCALIBRATIONMODE_COOKED); break;                              // CALIBRATIONMODE
      default: break;
    }
    if (g_log)
      patch::Log("xinput: pad %d: GetProperty(%s, obj 0x%lx how %lu) hr=0x%08lx", slot_ + 1, PropName(p), h->dwObj,
                 h->dwHow, static_cast<unsigned long>(hr));
    return hr;
  }

  STDMETHOD(SetProperty)(REFGUID prop, LPCDIPROPHEADER h) override {
    if (!h) return DIERR_INVALIDPARAM;
    const uintptr_t p = reinterpret_cast<uintptr_t>(&prop);
    Lock lock;
    const int obj = FindObject(h->dwObj, h->dwHow);
    const bool device = h->dwHow == DIPH_DEVICE;
    const DWORD data = h->dwSize >= sizeof(DIPROPDWORD) ? reinterpret_cast<const DIPROPDWORD*>(h)->dwData : 0;
    HRESULT hr = DI_OK;
    auto axes = [&](auto fn) {
      if (device) {
        for (int i = 0; i < kAxes; ++i) fn(i);
      } else if (obj >= 0 && obj < kAxes) {
        fn(obj);
      } else {
        hr = DIERR_OBJECTNOTFOUND;
      }
    };
    switch (p) {
      case 1:  // BUFFERSIZE
        if (acquired_) {
          hr = DIERR_ACQUIRED;
        } else {
          buffer_size_ = std::min<DWORD>(data, 1024);
          buffer_.clear();
        }
        break;
      case 4: {  // RANGE
        if (h->dwSize < sizeof(DIPROPRANGE)) {
          hr = DIERR_INVALIDPARAM;
          break;
        }
        const auto* r = reinterpret_cast<const DIPROPRANGE*>(h);
        if (r->lMin >= r->lMax) {
          hr = DIERR_INVALIDPARAM;
          break;
        }
        axes([&](int i) {
          min_[i] = r->lMin;
          max_[i] = r->lMax;
        });
        if (g_log) patch::Log("xinput: pad %d: SetProperty(RANGE %ld..%ld, obj 0x%lx how %lu)", slot_ + 1, r->lMin, r->lMax,
                              h->dwObj, h->dwHow);
        break;
      }
      case 5: axes([&](int i) { dz_[i] = std::min<DWORD>(data, 10000); }); break;   // DEADZONE
      case 6: axes([&](int i) { sat_[i] = std::min<DWORD>(data, 10000); }); break;  // SATURATION
      case 7: ff_gain_ = std::min<DWORD>(data, 10000); break;                       // FFGAIN
      case 9: autocenter_ = data; break;                                            // AUTOCENTER
      case 2: case 10: break;                                                       // AXISMODE, CALIBRATIONMODE
      default: hr = DIERR_UNSUPPORTED; break;
    }
    if (g_log && p != 4)
      patch::Log("xinput: pad %d: SetProperty(%s = %lu, obj 0x%lx how %lu) hr=0x%08lx", slot_ + 1, PropName(p), data,
                 h->dwObj, h->dwHow, static_cast<unsigned long>(hr));
    return hr;
  }

  STDMETHOD(Acquire)() override {
    Lock lock;
    if (!format_set_) return DIERR_INVALIDPARAM;
    if (acquired_) return S_FALSE;
    acquired_ = true;
    Sample(false);  // baseline: no events for what is already held
    patch::Log("xinput: pad %d acquired (%s, buffer %lu)", slot_ + 1, Connected() ? "connected" : "not connected",
               buffer_size_);
    return DI_OK;
  }

  STDMETHOD(Unacquire)() override {
    Lock lock;
    if (!acquired_) return S_FALSE;
    acquired_ = false;
    StopAll();
    SetMotors(0, 0);
    if (g_log) patch::Log("xinput: pad %d unacquired", slot_ + 1);
    return DI_OK;
  }

  STDMETHOD(GetDeviceState)(DWORD size, LPVOID data) override {
    if (!data) return DIERR_INVALIDPARAM;
    Lock lock;
    if (!acquired_) return DIERR_NOTACQUIRED;
    if (size != format_size_) return DIERR_INVALIDPARAM;
    Sample(true);
    BYTE* out = static_cast<BYTE*>(data);
    std::memset(out, 0, size);
    for (DWORD ofs : unmapped_povs_) *reinterpret_cast<DWORD*>(out + ofs) = 0xFFFFFFFF;
    for (int i = 0; i < kObjects; ++i) {
      if (ofs_[i] < 0) continue;
      if (i >= kObjButton0)
        out[ofs_[i]] = static_cast<BYTE>(last_[i]);
      else
        *reinterpret_cast<LONG*>(out + ofs_[i]) = last_[i];
    }
    ++stats_reads_;
    return DI_OK;
  }

  STDMETHOD(GetDeviceData)(DWORD obj_size, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inout, DWORD flags) override {
    if (!inout || obj_size < 16) return DIERR_INVALIDPARAM;
    Lock lock;
    if (!acquired_) return DIERR_NOTACQUIRED;
    if (buffer_size_ == 0) return DIERR_NOTBUFFERED;
    Sample(true);
    const bool peek = (flags & DIGDD_PEEK) != 0;
    HRESULT hr = overflow_ ? DI_BUFFEROVERFLOW : DI_OK;
    if (!peek) overflow_ = false;
    if (!rgdod) {
      DWORD n = *inout == INFINITE ? static_cast<DWORD>(buffer_.size())
                                   : std::min<DWORD>(*inout, static_cast<DWORD>(buffer_.size()));
      if (!peek) buffer_.erase(buffer_.begin(), buffer_.begin() + n);
      *inout = n;
      return hr;
    }
    const DWORD n = std::min<DWORD>(*inout, static_cast<DWORD>(buffer_.size()));
    BYTE* out = reinterpret_cast<BYTE*>(rgdod);
    for (DWORD i = 0; i < n; ++i) {
      std::memset(out + i * obj_size, 0, obj_size);
      std::memcpy(out + i * obj_size, &buffer_[i], 16);
    }
    if (!peek) buffer_.erase(buffer_.begin(), buffer_.begin() + n);
    *inout = n;
    stats_events_read_ += n;
    return hr;
  }

  STDMETHOD(SetDataFormat)(LPCDIDATAFORMAT df) override {
    if (!df || !df->rgodf || df->dwObjSize < sizeof(DIOBJECTDATAFORMAT)) return DIERR_INVALIDPARAM;
    Lock lock;
    if (acquired_) return DIERR_ACQUIRED;
    std::fill(std::begin(ofs_), std::end(ofs_), -1);
    unmapped_povs_.clear();
    int mapped = 0;
    for (DWORD k = 0; k < df->dwNumObjs; ++k) {
      const auto* odf = reinterpret_cast<const DIOBJECTDATAFORMAT*>(reinterpret_cast<const BYTE*>(df->rgodf) +
                                                                     k * df->dwObjSize);
      const DWORD types = DIDFT_GETTYPE(odf->dwType);
      const bool any = (odf->dwType & DIDFT_ANYINSTANCE) == DIDFT_ANYINSTANCE;
      const DWORD inst = DIDFT_GETINSTANCE(odf->dwType);
      int found = -1;
      for (int i = 0; i < kObjects && found < 0; ++i) {
        if (ofs_[i] >= 0 || !ObjPresent(i)) continue;
        if (types && !(kObj[i].type & types)) continue;
        if (odf->pguid && !Same(*odf->pguid, *kObj[i].guid)) continue;
        if (!any && kObj[i].instance != inst) continue;
        found = i;
      }
      const DWORD width = found >= kObjButton0 ? 1 : 4;
      if (found >= 0 && odf->dwOfs + width > df->dwDataSize) found = -1;
      if (found >= 0) {
        ofs_[found] = static_cast<int>(odf->dwOfs);
        ++mapped;
      } else if ((types & DIDFT_POV) && odf->dwOfs + 4 <= df->dwDataSize) {
        unmapped_povs_.push_back(odf->dwOfs);  // a POV we don't have reads as centred
      }
    }
    format_size_ = df->dwDataSize;
    format_set_ = true;
    patch::Log("xinput: pad %d: data format %lu bytes, %lu objects, %d of ours mapped", slot_ + 1, df->dwDataSize,
               df->dwNumObjs, mapped);
    return DI_OK;
  }

  STDMETHOD(SetEventNotification)(HANDLE ev) override {
    Lock lock;
    event_ = ev;
    return DI_OK;
  }
  STDMETHOD(SetCooperativeLevel)(HWND hwnd, DWORD flags) override {
    if (g_log) patch::Log("xinput: pad %d: SetCooperativeLevel(%p, 0x%lx)", slot_ + 1, static_cast<void*>(hwnd), flags);
    return DI_OK;
  }

  STDMETHOD(GetObjectInfo)(LPDIDEVICEOBJECTINSTANCEA out, DWORD obj, DWORD how) override {
    if (!out || out->dwSize < sizeof(DIDEVICEOBJECTINSTANCE_DX3A)) return DIERR_INVALIDPARAM;
    Lock lock;
    const int i = FindObject(obj, how);
    if (i < 0) return DIERR_OBJECTNOTFOUND;
    const DWORD size = out->dwSize;
    DIDEVICEOBJECTINSTANCEA o;
    FillObject(i, &o);
    std::memcpy(out, &o, std::min<DWORD>(size, sizeof o));
    out->dwSize = size;
    return DI_OK;
  }

  STDMETHOD(GetDeviceInfo)(LPDIDEVICEINSTANCEA out) override {
    if (!out || out->dwSize < sizeof(DIDEVICEINSTANCE_DX3A)) return DIERR_INVALIDPARAM;
    const DWORD size = out->dwSize;
    DIDEVICEINSTANCEA d;
    FillInstance(slot_, &d);
    std::memcpy(out, &d, std::min<DWORD>(size, sizeof d));
    out->dwSize = size;
    return DI_OK;
  }

  STDMETHOD(RunControlPanel)(HWND, DWORD) override { return DIERR_UNSUPPORTED; }
  STDMETHOD(Initialize)(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }

  // IDirectInputDevice2
  STDMETHOD(CreateEffect)(REFGUID type, LPCDIEFFECT eff, LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) override {
    if (!out) return DIERR_INVALIDPARAM;
    *out = nullptr;
    if (outer) return DIERR_NOAGGREGATION;
    if (!RumbleOn() || !EffectKnown(type)) {
      patch::Log("xinput: pad %d: CreateEffect %s refused", slot_ + 1, GuidText(type).c_str());
      return DIERR_DEVICENOTREG;
    }
    auto* e = new Effect(this, type);
    if (eff) {
      HRESULT hr = e->SetParameters(eff, DIEP_ALLPARAMS);
      if (FAILED(hr)) {
        e->Release();
        return hr;
      }
    }
    *out = e;
    patch::Log("xinput: pad %d: CreateEffect %s -> %p", slot_ + 1, EffectName(type), static_cast<void*>(e));
    return DI_OK;
  }

  STDMETHOD(EnumEffects)(LPDIENUMEFFECTSCALLBACKA cb, LPVOID ref, DWORD type) override {
    if (!cb) return DIERR_INVALIDPARAM;
    if (!RumbleOn()) return DI_OK;
    const GUID* list[] = {&kGUID_ConstantForce, &kGUID_RampForce, &kGUID_Square, &kGUID_Sine,
                          &kGUID_Triangle, &kGUID_SawtoothUp, &kGUID_SawtoothDown};
    for (const GUID* g : list) {
      DIEFFECTINFOA info;
      FillEffectInfo(*g, &info);
      if (DIEFT_GETTYPE(type) != DIEFT_ALL && DIEFT_GETTYPE(type) != DIEFT_GETTYPE(info.dwEffType)) continue;
      if (cb(&info, ref) == DIENUM_STOP) break;
    }
    return DI_OK;
  }

  STDMETHOD(GetEffectInfo)(LPDIEFFECTINFOA info, REFGUID type) override {
    if (!info || info->dwSize < sizeof(DIEFFECTINFOA)) return DIERR_INVALIDPARAM;
    if (!RumbleOn() || !EffectKnown(type)) return DIERR_DEVICENOTREG;
    FillEffectInfo(type, info);
    return DI_OK;
  }

  STDMETHOD(GetForceFeedbackState)(LPDWORD out) override {
    if (!out) return DIERR_INVALIDPARAM;
    if (!RumbleOn()) return DIERR_UNSUPPORTED;
    Lock lock;
    DWORD s = DIGFFS_POWERON | (actuators_ ? DIGFFS_ACTUATORSON : DIGFFS_ACTUATORSOFF);
    if (effects_.empty()) s |= DIGFFS_EMPTY;
    bool any = false;
    for (Effect* e : effects_) any = any || e->playing_;
    if (!any) s |= DIGFFS_STOPPED;
    if (paused_) s |= DIGFFS_PAUSED;
    *out = s;
    return DI_OK;
  }

  STDMETHOD(SendForceFeedbackCommand)(DWORD cmd) override {
    if (!RumbleOn()) return DIERR_UNSUPPORTED;
    Lock lock;
    switch (cmd) {
      case DISFFC_RESET:
      case DISFFC_STOPALL: StopAll(); paused_ = false; break;
      case DISFFC_PAUSE: paused_ = true; break;
      case DISFFC_CONTINUE: paused_ = false; break;
      case DISFFC_SETACTUATORSON: actuators_ = true; break;
      case DISFFC_SETACTUATORSOFF: actuators_ = false; break;
      default: return DIERR_INVALIDPARAM;
    }
    if (g_log) patch::Log("xinput: pad %d: SendForceFeedbackCommand(0x%lx)", slot_ + 1, cmd);
    UpdateRumble(NowUs());
    return DI_OK;
  }

  STDMETHOD(EnumCreatedEffectObjects)(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD) override {
    if (!cb) return DIERR_INVALIDPARAM;
    std::vector<Effect*> copy;
    {
      Lock lock;
      copy = effects_;
    }
    for (Effect* e : copy)
      if (cb(e, ref) == DIENUM_STOP) break;
    return DI_OK;
  }

  STDMETHOD(Escape)(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }

  STDMETHOD(Poll)() override {
    Lock lock;
    if (!acquired_) return DIERR_NOTACQUIRED;
    Sample(true);
    return DI_OK;
  }

  STDMETHOD(SendDeviceData)(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_UNSUPPORTED; }
  STDMETHOD(EnumEffectsInFile)(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override {
    return DIERR_UNSUPPORTED;
  }
  STDMETHOD(WriteEffectToFile)(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }

  // ------------------------------------------------------------ helpers

  static DWORD DevType() { return DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_TRADITIONAL << 8); }

  static void FillInstance(int slot, DIDEVICEINSTANCEA* d) {
    std::memset(d, 0, sizeof *d);
    d->dwSize = sizeof *d;
    d->guidInstance = PadInstanceGuid(slot);
    d->guidProduct = kPadProduct;
    d->dwDevType = DevType();
    if (!g_slot_name[slot].empty()) {
      std::snprintf(d->tszInstanceName, sizeof d->tszInstanceName, "%s", g_slot_name[slot].c_str());
      std::snprintf(d->tszProductName, sizeof d->tszProductName, "%s", g_slot_name[slot].c_str());
    } else {
      std::snprintf(d->tszInstanceName, sizeof d->tszInstanceName, "Xbox Controller %d", slot + 1);
      std::snprintf(d->tszProductName, sizeof d->tszProductName, "Xbox Controller %d (XInput)", slot + 1);
    }
    if (RumbleOn()) d->guidFFDriver = kPadProduct;
    d->wUsagePage = 1;  // generic desktop
    d->wUsage = 4;      // joystick
  }

  void FillObject(int i, DIDEVICEOBJECTINSTANCEA* o) const {
    std::memset(o, 0, sizeof *o);
    o->dwSize = sizeof *o;
    o->guidType = *kObj[i].guid;
    o->dwOfs = ofs_[i] >= 0 ? static_cast<DWORD>(ofs_[i]) : kObj[i].default_ofs;
    o->dwType = ObjType(i);
    o->dwFlags = i < kAxes ? DIDOI_ASPECTPOSITION : 0;
    if (o->dwType & DIDFT_FFACTUATOR) {
      o->dwFlags |= DIDOI_FFACTUATOR;
      o->dwFFMaxForce = 10000;
      o->dwFFForceResolution = 1;
    }
    std::snprintf(o->tszName, sizeof o->tszName, "%s", kObj[i].name);
    o->wUsagePage = i < kAxes ? 1 : i == kObjPov ? 1 : 9;
    static const WORD axis_usage[kAxes] = {0x30, 0x31, 0x32, 0x33, 0x34, 0x35};
    o->wUsage = i < kAxes ? axis_usage[i] : i == kObjPov ? 0x39 : static_cast<WORD>(kObj[i].instance + 1);
  }

  // Object index for a property / object-info request, or -1.
  int FindObject(DWORD obj, DWORD how) const {
    if (how == DIPH_BYOFFSET) {
      for (int i = 0; i < kObjects; ++i)
        if (ofs_[i] >= 0 && static_cast<DWORD>(ofs_[i]) == obj) return i;
      return -1;
    }
    if (how == DIPH_BYID) {
      const DWORD types = DIDFT_GETTYPE(obj);
      const DWORD inst = DIDFT_GETINSTANCE(obj);
      for (int i = 0; i < kObjects; ++i)
        if (ObjPresent(i) && (kObj[i].type & types) && kObj[i].instance == inst) return i;
      return -1;
    }
    return -1;
  }

  static bool EffectKnown(REFGUID g) {
    return Same(g, kGUID_ConstantForce) || Same(g, kGUID_RampForce) || Same(g, kGUID_Square) || Same(g, kGUID_Sine) ||
           Same(g, kGUID_Triangle) || Same(g, kGUID_SawtoothUp) || Same(g, kGUID_SawtoothDown);
  }

  static const char* EffectName(REFGUID g) {
    if (Same(g, kGUID_ConstantForce)) return "ConstantForce";
    if (Same(g, kGUID_RampForce)) return "RampForce";
    if (Same(g, kGUID_Square)) return "Square";
    if (Same(g, kGUID_Sine)) return "Sine";
    if (Same(g, kGUID_Triangle)) return "Triangle";
    if (Same(g, kGUID_SawtoothUp)) return "SawtoothUp";
    if (Same(g, kGUID_SawtoothDown)) return "SawtoothDown";
    return "unknown";
  }

  static void FillEffectInfo(REFGUID g, DIEFFECTINFOA* info) {
    std::memset(info, 0, sizeof *info);
    info->dwSize = sizeof *info;
    info->guid = g;
    DWORD type = Same(g, kGUID_ConstantForce) ? DIEFT_CONSTANTFORCE : Same(g, kGUID_RampForce) ? DIEFT_RAMPFORCE
                                                                                                : DIEFT_PERIODIC;
    info->dwEffType = type | DIEFT_FFATTACK | DIEFT_FFFADE;
    info->dwStaticParams = info->dwDynamicParams = DIEP_DURATION | DIEP_GAIN | DIEP_AXES | DIEP_DIRECTION |
                                                   DIEP_ENVELOPE | DIEP_TYPESPECIFICPARAMS | DIEP_STARTDELAY;
    std::snprintf(info->tszName, sizeof info->tszName, "%s", EffectName(g));
  }

  bool Connected() const { return (g_connected_mask >> slot_) & 1; }

  // Read the pad, update last_[], queue buffered events for changes. g_cs held.
  void Sample(bool events) {
    XINPUT_STATE st;
    std::memset(&st, 0, sizeof st);
    const DWORD r = g_get_state ? g_get_state(static_cast<DWORD>(slot_), &st) : ERROR_DEVICE_NOT_CONNECTED;
    const bool now_connected = r == ERROR_SUCCESS;
    if (now_connected != Connected()) {
      g_connected_mask = now_connected ? (g_connected_mask | (1u << slot_)) : (g_connected_mask & ~(1u << slot_));
      patch::Log("xinput: controller %d %s", slot_ + 1, now_connected ? "connected" : "disconnected");
    }
    if (!now_connected) std::memset(&st, 0, sizeof st);  // neutral while unplugged
    const bool same = now_connected && st.dwPacketNumber == packet_ && have_packet_;
    if (same && !smoothing_) return;  // nothing new (unless steering is still catching up)
    if (!same) {
      packet_ = st.dwPacketNumber;
      have_packet_ = now_connected;
      ++stats_packets_;
    }

    const XINPUT_GAMEPAD& g = st.Gamepad;
    float lx, ly, rx, ry;
    Stick(g.sThumbLX, g.sThumbLY, &lx, &ly);
    // XInputSteerCurve on the left stick's X (steering). The game already
    // cubes joystick steering (0x52e3e0: steer = x^3), so 33 gives a straight
    // line from stick to bike and 100 leaves the game's own curve.
    if (g_curve != 1.0f) lx = std::copysign(std::pow(std::fabs(lx), g_curve), lx);
    // XInputSteerSmooth: steering eases toward the stick instead of jumping,
    // which takes the edge off small thumb flicks.
    if (g_steer_smooth_us > 0) {
      const double now = NowUs();
      const double dt = smooth_us_ > 0 ? std::min(100000.0, now - smooth_us_) : 100000.0;
      smooth_us_ = now;
      const float a = static_cast<float>(1.0 - std::exp(-dt / g_steer_smooth_us));
      smooth_lx_ += (lx - smooth_lx_) * a;
      if (std::fabs(lx - smooth_lx_) < 0.002f) smooth_lx_ = lx;
      smoothing_ = smooth_lx_ != lx;
      lx = smooth_lx_;
    }
    Stick(g.sThumbRX, g.sThumbRY, &rx, &ry);
    const float lt = g.bLeftTrigger / 255.0f, rt = g.bRightTrigger / 255.0f;
    LONG v[kObjects];
    v[0] = AxisCentered(0, lx);
    v[1] = AxisCentered(1, -ly);
    v[2] = AxisTrigger(2, rt, true);  // the game reads Z as max - Z
    v[3] = AxisCentered(3, rx);
    v[4] = AxisCentered(4, -ry);
    v[5] = AxisTrigger(5, lt, false);
    v[kObjPov] = static_cast<LONG>(Pov(g.wButtons));
    const WORD kMasks[10] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                             XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK,
                             XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB};
    for (int b = 0; b < 10; ++b) v[kObjButton0 + b] = (g.wButtons & kMasks[b]) ? 0x80 : 0;
    v[kObjButton0 + 10] = g_trigger_buttons && g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 0x80 : 0;
    v[kObjButton0 + 11] = g_trigger_buttons && g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 0x80 : 0;

    const DWORD stamp = GetTickCount();
    bool changed = false;
    for (int i = 0; i < kObjects; ++i) {
      if (v[i] == last_[i]) continue;
      changed = true;
      if (events && buffer_size_ > 0 && ofs_[i] >= 0) {
        if (buffer_.size() >= buffer_size_) {
          buffer_.pop_front();
          overflow_ = true;
          ++stats_overflows_;
        }
        DIDEVICEOBJECTDATA e = {};
        e.dwOfs = static_cast<DWORD>(ofs_[i]);
        e.dwData = static_cast<DWORD>(v[i]);
        e.dwTimeStamp = stamp;
        e.dwSequence = ++g_sequence;
        buffer_.push_back(e);
        ++stats_events_;
      }
      if (i >= kObjButton0 && g_log && events && (v[i] != 0) != (last_[i] != 0))
        patch::Log("xinput: pad %d: %s %s", slot_ + 1, kObj[i].name, v[i] ? "down" : "up");
      last_[i] = v[i];
    }
    if (changed && events && event_) SetEvent(event_);
  }

  // Radial dead zone (XInputDeadzone) on a stick, result in -1..1.
  static void Stick(SHORT sx, SHORT sy, float* ox, float* oy) {
    float x = std::max(-1.0f, sx / 32767.0f), y = std::max(-1.0f, sy / 32767.0f);
    const float mag = std::sqrt(x * x + y * y);
    if (mag <= g_deadzone || mag <= 0.0f) {
      *ox = *oy = 0;
      return;
    }
    const float scaled = (std::min(mag, 1.0f) - g_deadzone) / (1.0f - g_deadzone);
    *ox = std::max(-1.0f, std::min(1.0f, x / mag * scaled));
    *oy = std::max(-1.0f, std::min(1.0f, y / mag * scaled));
  }

  // The game's own DIPROP_DEADZONE / SATURATION on a 0..1 magnitude.
  float Shape(int axis, float a) const {
    const float dz = dz_[axis] / 10000.0f, sat = std::max(dz + 0.0001f, sat_[axis] / 10000.0f);
    if (a <= dz) return 0;
    if (a >= sat) return 1;
    return (a - dz) / (sat - dz);
  }

  LONG AxisCentered(int axis, float n) const {
    const float s = Shape(axis, std::fabs(n));
    const float u = (n < 0 ? -s : s + 0.0f) * 0.5f + 0.5f;  // 0..1
    return min_[axis] + static_cast<LONG>(std::lround(u * static_cast<double>(max_[axis] - min_[axis])));
  }

  // A trigger reads from one end of its axis (rest) to the other. The
  // game's Accelerate uses the whole Z axis (xinput-4 rested triggers at the
  // centre: half or more throttle with nothing pressed).
  LONG AxisTrigger(int axis, float t, bool inverted) const {
    const float s = Shape(axis, t);
    const LONG span = static_cast<LONG>(std::lround(s * static_cast<double>(max_[axis] - min_[axis])));
    return inverted ? max_[axis] - span : min_[axis] + span;
  }

  static DWORD Pov(WORD b) {
    int x = ((b & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : 0) - ((b & XINPUT_GAMEPAD_DPAD_LEFT) ? 1 : 0);
    int y = ((b & XINPUT_GAMEPAD_DPAD_UP) ? 1 : 0) - ((b & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : 0);
    if (!x && !y) return 0xFFFFFFFF;
    static const DWORD table[3][3] = {{31500, 27000, 22500}, {0, 0xFFFFFFFF, 18000}, {4500, 9000, 13500}};
    // table[x+1][...]: y = 1 up, 0, -1 down
    return table[x + 1][1 - y];
  }

  void StopAll() {
    for (Effect* e : effects_) e->playing_ = false;
  }

  void SetMotors(WORD low, WORD high) {
    if (!g_set_state) return;
    if (low == motor_low_ && high == motor_high_) return;
    motor_low_ = low;
    motor_high_ = high;
    XINPUT_VIBRATION v = {low, high};
    g_set_state(static_cast<DWORD>(slot_), &v);
    if (low > stats_peak_low_) stats_peak_low_ = low;
    if (high > stats_peak_high_) stats_peak_high_ = high;
    ++stats_motor_changes_;
  }

  // Mix the playing effects into the two motors. g_cs held.
  void UpdateRumble(double now_us) {
    float low = 0, high = 0;
    for (Effect* e : effects_) e->Strength(now_us, &low, &high);
    bool fg = false;
    if (HWND w = GetForegroundWindow()) {
      DWORD pid = 0;
      GetWindowThreadProcessId(w, &pid);
      fg = pid == GetCurrentProcessId();
    }
    if (!acquired_ || !actuators_ || paused_ || !fg) low = high = 0;
    const float k = static_cast<float>(ff_gain_) / 10000.0f * static_cast<float>(g_rumble_pct) / 100.0f;
    // Pad motors barely spin below about a fifth of full power, and the
    // game's bumps are small forces: a square root curve makes them felt.
    // Both motors run for low effects (the light one adds the sharp edge).
    auto motor = [&](float s) {
      const float x = std::min(1.0f, s * k);
      return static_cast<WORD>(std::lround((x > 0 ? std::sqrt(x) : 0.0f) * 65535.0f));
    };
    SetMotors(motor(low), motor(std::max(high, low * 0.6f)));
  }

  void LogSummary() {
    patch::Log("xinput: pad %d: %s, %s, axes %ld %ld %ld %ld %ld %ld, pov %ld, %lu packets, %lu events (%lu read, "
               "%lu dropped), %lu state reads, rumble peak %u/%u (%lu changes), %u effects",
               slot_ + 1, Connected() ? "connected" : "not connected", acquired_ ? "acquired" : "not acquired", last_[0],
               last_[1], last_[2], last_[3], last_[4], last_[5], last_[kObjPov], stats_packets_, stats_events_,
               stats_events_read_, stats_overflows_, stats_reads_, static_cast<unsigned>(stats_peak_low_),
               static_cast<unsigned>(stats_peak_high_), stats_motor_changes_, static_cast<unsigned>(effects_.size()));
    stats_packets_ = stats_events_ = stats_events_read_ = stats_overflows_ = stats_reads_ = stats_motor_changes_ = 0;
    stats_peak_low_ = stats_peak_high_ = 0;
  }

  int slot_;
  bool acquired_ = false, format_set_ = false;
  DWORD format_size_ = 0;
  int ofs_[kObjects];
  std::vector<DWORD> unmapped_povs_;
  LONG last_[kObjects];
  LONG min_[kAxes], max_[kAxes];
  DWORD dz_[kAxes], sat_[kAxes];
  DWORD buffer_size_ = 0;
  std::deque<DIDEVICEOBJECTDATA> buffer_;
  bool overflow_ = false;
  DWORD packet_ = 0;
  bool have_packet_ = false;
  HANDLE event_ = nullptr;
  DWORD ff_gain_ = 10000, autocenter_ = 0;
  bool actuators_ = true, paused_ = false;
  std::vector<Effect*> effects_;
  WORD motor_low_ = 0, motor_high_ = 0;
  float smooth_lx_ = 0;
  double smooth_us_ = 0;
  bool smoothing_ = false;
  // stats for XInputLog
  DWORD stats_packets_ = 0, stats_events_ = 0, stats_events_read_ = 0, stats_overflows_ = 0, stats_reads_ = 0,
        stats_motor_changes_ = 0;
  WORD stats_peak_low_ = 0, stats_peak_high_ = 0;
  static DWORD g_sequence;

 private:
  volatile LONG refs_ = 1;
};

DWORD Pad::g_sequence = 0;

// ---------------------------------------------------------------- effect impl
Effect::Effect(Pad* pad, REFGUID type) : pad_(pad), type_(type) {
  periodic_ = !Same(type, kGUID_ConstantForce) && !Same(type, kGUID_RampForce);
  ramp_ = Same(type, kGUID_RampForce);
  pad_->AddRef();
  Lock lock;
  pad_->effects_.push_back(this);
}

Effect::~Effect() {
  {
    Lock lock;
    auto& v = pad_->effects_;
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
    pad_->UpdateRumble(NowUs());
  }
  pad_->Release();
}

HRESULT Effect::SetParameters(LPCDIEFFECT eff, DWORD flags) {
  if (!eff || eff->dwSize < sizeof(DIEFFECT_DX5)) return DIERR_INVALIDPARAM;
  Lock lock;
  if (flags & DIEP_DURATION) {
    duration_ = eff->dwDuration;
    steady_ = !periodic_ && !ramp_ && duration_ == INFINITE;
  }
  if (flags & DIEP_GAIN) gain_ = std::min<DWORD>(eff->dwGain, 10000);
  if (flags & DIEP_AXES) axes_ = eff->cAxes;
  if (flags & DIEP_STARTDELAY) start_delay_ = eff->dwSize >= sizeof(DIEFFECT) ? eff->dwStartDelay : 0;
  if (flags & DIEP_ENVELOPE) {
    has_env_ = eff->lpEnvelope != nullptr;
    if (has_env_) env_ = *eff->lpEnvelope;
  }
  if ((flags & DIEP_TYPESPECIFICPARAMS) && eff->lpvTypeSpecificParams) {
    if (periodic_ && eff->cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
      const auto* p = static_cast<const DIPERIODIC*>(eff->lpvTypeSpecificParams);
      magnitude_ = static_cast<LONG>(std::min<DWORD>(p->dwMagnitude, 10000));
      offset_ = p->lOffset;
      period_ = p->dwPeriod;
    } else if (ramp_ && eff->cbTypeSpecificParams >= sizeof(DIRAMPFORCE)) {
      const auto* p = static_cast<const DIRAMPFORCE*>(eff->lpvTypeSpecificParams);
      magnitude_ = p->lStart;
      ramp_end_ = p->lEnd;
    } else if (!periodic_ && !ramp_ && eff->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE)) {
      const LONG m = static_cast<const DICONSTANTFORCE*>(eff->lpvTypeSpecificParams)->lMagnitude;
      const double jump = std::fabs(static_cast<double>(m) - magnitude_);
      if (jump >= g_kick_threshold) {
        Kick(NowUs(), jump, kKickUs);
        if (g_log && steady_ && !g_rumble_forces)
          patch::Log("xinput: effect %p (ConstantForce): force jump %ld -> %ld, rumble kick %.0f",
                     static_cast<void*>(this), magnitude_, m, kick_);
      }
      magnitude_ = m;
    } else {
      return DIERR_INVALIDPARAM;
    }
  }
  if (g_log)
    patch::Log("xinput: effect %p (%s): SetParameters flags 0x%lx: duration %lu us, gain %lu, magnitude %ld, end %ld, "
               "offset %ld, period %lu us, envelope %s (attack %lu/%lu us, fade %lu/%lu us)",
               static_cast<void*>(this), Pad::EffectName(type_), flags, duration_, gain_, magnitude_, ramp_end_,
               offset_, period_, has_env_ ? "yes" : "no", env_.dwAttackLevel, env_.dwAttackTime, env_.dwFadeLevel,
               env_.dwFadeTime);
  if (flags & DIEP_START) {
    playing_ = true;
    iterations_ = 1;
    start_us_ = NowUs();
  }
  pad_->UpdateRumble(NowUs());
  return DI_OK;
}

HRESULT Effect::GetParameters(LPDIEFFECT eff, DWORD flags) {
  if (!eff || eff->dwSize < sizeof(DIEFFECT_DX5)) return DIERR_INVALIDPARAM;
  Lock lock;
  if (flags & DIEP_DURATION) eff->dwDuration = duration_;
  if (flags & DIEP_GAIN) eff->dwGain = gain_;
  if (flags & DIEP_SAMPLEPERIOD) eff->dwSamplePeriod = 0;
  if (flags & DIEP_TRIGGERBUTTON) eff->dwTriggerButton = DIEB_NOTRIGGER;
  if (flags & DIEP_TRIGGERREPEATINTERVAL) eff->dwTriggerRepeatInterval = 0;
  if ((flags & DIEP_STARTDELAY) && eff->dwSize >= sizeof(DIEFFECT)) eff->dwStartDelay = start_delay_;
  if ((flags & DIEP_ENVELOPE) && eff->lpEnvelope) {
    if (has_env_) {
      *eff->lpEnvelope = env_;
    } else {
      eff->lpEnvelope = nullptr;
    }
  }
  if ((flags & DIEP_TYPESPECIFICPARAMS) && eff->lpvTypeSpecificParams) {
    if (periodic_ && eff->cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
      auto* p = static_cast<DIPERIODIC*>(eff->lpvTypeSpecificParams);
      p->dwMagnitude = static_cast<DWORD>(magnitude_);
      p->lOffset = offset_;
      p->dwPhase = 0;
      p->dwPeriod = period_;
    } else if (ramp_ && eff->cbTypeSpecificParams >= sizeof(DIRAMPFORCE)) {
      auto* p = static_cast<DIRAMPFORCE*>(eff->lpvTypeSpecificParams);
      p->lStart = magnitude_;
      p->lEnd = ramp_end_;
    } else if (eff->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE)) {
      static_cast<DICONSTANTFORCE*>(eff->lpvTypeSpecificParams)->lMagnitude = magnitude_;
    }
  }
  return DI_OK;
}

HRESULT Effect::Start(DWORD iterations, DWORD flags) {
  Lock lock;
  if (flags & DIES_SOLO)
    for (Effect* e : pad_->effects_) e->playing_ = false;
  playing_ = true;
  iterations_ = iterations ? iterations : 1;
  start_us_ = NowUs();
  if (g_log)
    patch::Log("xinput: effect %p (%s): Start(%lu, 0x%lx)", static_cast<void*>(this), Pad::EffectName(type_), iterations,
               flags);
  pad_->UpdateRumble(start_us_);
  return DI_OK;
}

HRESULT Effect::Stop() {
  Lock lock;
  if (g_log && playing_) patch::Log("xinput: effect %p (%s): Stop", static_cast<void*>(this), Pad::EffectName(type_));
  if (playing_ && steady_ && !g_rumble_forces) {
    Kick(NowUs(), std::max(kCrashKick, std::fabs(static_cast<double>(magnitude_))), kCrashUs);
    if (g_log) patch::Log("xinput: effect %p (ConstantForce): stopped, crash kick %.0f", static_cast<void*>(this), kick_);
  }
  playing_ = false;
  pad_->UpdateRumble(NowUs());
  return DI_OK;
}

HRESULT Effect::GetEffectStatus(LPDWORD status) {
  if (!status) return DIERR_INVALIDPARAM;
  Lock lock;
  float low = 0, high = 0;
  Strength(NowUs(), &low, &high);  // ends it if its time is up
  *status = playing_ ? DIEGES_PLAYING : 0;
  return DI_OK;
}

void Effect::Strength(double now_us, float* low, float* high) {
  if (steady_ && !g_rumble_forces) {  // only its kicks rumble, even after a Stop
    *low += static_cast<float>(KickLevel(now_us) / 10000.0 * gain_ / 10000.0);
    return;
  }
  if (!playing_) return;
  double t = now_us - start_us_ - start_delay_;
  if (t < 0) return;
  double local = t;
  if (duration_ != INFINITE) {
    if (duration_ == 0) {
      playing_ = false;
      return;
    }
    // A single short pulse is held to kMinPulseUs so the motors can feel it.
    const double span = iterations_ == 1 ? std::max<double>(duration_, kMinPulseUs) : duration_;
    const double it = std::floor(t / span);
    if (iterations_ != INFINITE && it >= iterations_) {
      playing_ = false;
      return;
    }
    local = std::min<double>(t - it * span, duration_);
  }
  double level;
  if (periodic_) {
    level = std::min(10000.0, static_cast<double>(magnitude_) + std::fabs(static_cast<double>(offset_)));
  } else if (ramp_ && duration_ != INFINITE) {
    level = std::fabs(magnitude_ + (ramp_end_ - magnitude_) * (local / duration_));
  } else {
    level = std::fabs(static_cast<double>(magnitude_));
  }
  if (has_env_) {
    if (env_.dwAttackTime > 0 && local < env_.dwAttackTime)
      level = env_.dwAttackLevel + (level - env_.dwAttackLevel) * (local / env_.dwAttackTime);
    if (duration_ != INFINITE && env_.dwFadeTime > 0 && local > static_cast<double>(duration_) - env_.dwFadeTime) {
      const double f = (local - (static_cast<double>(duration_) - env_.dwFadeTime)) / env_.dwFadeTime;
      level = level + (env_.dwFadeLevel - level) * std::min(1.0, f);
    }
  }
  const float s = static_cast<float>(std::max(0.0, level) / 10000.0 * gain_ / 10000.0);
  if (periodic_ && period_ > 0 && period_ < 50000)
    *high += s;  // faster than 20 Hz: the light motor
  else
    *low += s;
}

// ---------------------------------------------------------------- sampler thread
DWORD WINAPI SampleThread(void*) {
  DWORD last_summary = GetTickCount();
  for (;;) {
    bool active = false;
    {
      Lock lock;
      const double now = NowUs();
      for (Pad* p : g_pads) {
        if (!p->acquired_) continue;
        active = true;
        p->Sample(true);
        p->UpdateRumble(now);
      }
      if (g_log && GetTickCount() - last_summary >= 10000) {
        last_summary = GetTickCount();
        for (Pad* p : g_pads) p->LogSummary();
      }
    }
    Sleep(active ? 4 : 50);
  }
}

void StartThread() {
  if (g_thread) return;
  g_thread = CreateThread(nullptr, 0, &SampleThread, nullptr, 0, nullptr);
  if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_ABOVE_NORMAL);
  patch::Log("xinput: sampling thread %s (every 4 ms while a pad is acquired)", g_thread ? "started" : "FAILED");
}

// ---------------------------------------------------------------- hiding Windows' copies
struct VidPid {
  WORD vid, pid;
  bool operator<(const VidPid& o) const { return vid != o.vid ? vid < o.vid : pid < o.pid; }
};

std::map<VidPid, std::string> g_hid_names;  // XInput HID devices' product strings

std::string HidProductName(const char* path) {
  using GetStrFn = BOOLEAN(WINAPI*)(HANDLE, PVOID, ULONG);
  static GetStrFn get = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    if (HMODULE h = LoadLibraryA("hid.dll"))
      get = reinterpret_cast<GetStrFn>(reinterpret_cast<void*>(GetProcAddress(h, "HidD_GetProductString")));
  }
  if (!get) return "";
  HANDLE f = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return "";
  wchar_t w[128] = {};
  std::string out;
  if (get(f, w, sizeof w - sizeof(wchar_t))) {
    char a[256] = {};
    WideCharToMultiByte(CP_ACP, 0, w, -1, a, sizeof a - 1, nullptr, nullptr);
    out = a;
    while (!out.empty() && out.back() == ' ') out.pop_back();
  }
  CloseHandle(f);
  return out;
}

std::set<VidPid> XInputHidDevices() {
  std::set<VidPid> out;
  UINT n = 0;
  if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || n == 0) return out;
  std::vector<RAWINPUTDEVICELIST> list(n);
  n = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
  if (n == static_cast<UINT>(-1)) return out;
  for (UINT i = 0; i < n; ++i) {
    if (list[i].dwType != RIM_TYPEHID) continue;
    char name[512] = {};
    UINT len = sizeof name;
    if (GetRawInputDeviceInfoA(list[i].hDevice, RIDI_DEVICENAME, name, &len) == static_cast<UINT>(-1)) continue;
    std::string s(name);
    for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (s.find("IG_") == std::string::npos) continue;
    const size_t v = s.find("VID_"), p = s.find("PID_");
    if (v == std::string::npos || p == std::string::npos) continue;
    VidPid vp = {static_cast<WORD>(strtoul(s.substr(v + 4, 4).c_str(), nullptr, 16)),
                 static_cast<WORD>(strtoul(s.substr(p + 4, 4).c_str(), nullptr, 16))};
    if (out.insert(vp).second) {
      g_hid_names[vp] = HidProductName(name);
      patch::Log("xinput: XInput HID device VID_%04X PID_%04X \"%s\" (%s)", vp.vid, vp.pid, g_hid_names[vp].c_str(),
                 name);
    }
  }
  return out;
}

// "Wooting Two HE (XInput 2)", or "Xbox Controller 1" when the device can't be told.
void NameSlot(int slot) {
  std::string name;
  XCapsEx ex = {};
  XINPUT_CAPABILITIES caps = {};
  bool have_ids = g_caps_ex && g_caps_ex(1, static_cast<DWORD>(slot), 0, &ex) == ERROR_SUCCESS;
  if (have_ids) caps = ex.caps;
  else if (g_caps) g_caps(static_cast<DWORD>(slot), 0, &caps);
  if (have_ids) {
    auto it = g_hid_names.find(VidPid{ex.vid, ex.pid});
    if (it != g_hid_names.end() && !it->second.empty()) {
      char b[200];
      std::snprintf(b, sizeof b, "%s (XInput %d)", it->second.c_str(), slot + 1);
      name = b;
    } else if (ex.vid != 0x045E) {
      char b[200];
      std::snprintf(b, sizeof b, "XInput controller %d (VID_%04X PID_%04X)", slot + 1, ex.vid, ex.pid);
      name = b;
    }
  }
  if (name.empty()) name = "Xbox Controller " + std::to_string(slot + 1);
  g_slot_name[slot] = name;
  patch::Log("xinput: slot %d: \"%s\" (type %u subtype %u flags 0x%x%s)", slot + 1, name.c_str(), caps.Type,
             caps.SubType, caps.Flags,
             have_ids ? (" VID_" + [&] { char b[16]; std::snprintf(b, sizeof b, "%04X PID_%04X", ex.vid, ex.pid); return std::string(b); }()).c_str() : "");
}

bool IsXInputCopy(const DIDEVICEINSTANCEA* d, const std::set<VidPid>& hid) {
  const BYTE sig[8] = {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'};
  if (std::memcmp(d->guidProduct.Data4, sig, 8) != 0) return false;
  VidPid vp = {LOWORD(d->guidProduct.Data1), HIWORD(d->guidProduct.Data1)};
  return hid.count(vp) != 0;
}

// ---------------------------------------------------------------- IDirectInput7A wrapper
class DirectInputWrap final : public IDirectInput7A {
 public:
  explicit DirectInputWrap(IDirectInput7A* real) : real_(real) {}

  STDMETHOD(QueryInterface)(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (Same(riid, kIID_IUnknown) || Same(riid, kIID_DIA) || Same(riid, kIID_DI2A) || Same(riid, kIID_DI7A)) {
      *out = static_cast<IDirectInput7A*>(this);
      AddRef();
      return S_OK;
    }
    return real_->QueryInterface(riid, out);
  }
  STDMETHOD_(ULONG, AddRef)() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
  STDMETHOD_(ULONG, Release)() override {
    LONG r = InterlockedDecrement(&refs_);
    if (r == 0) {
      real_->Release();
      delete this;
    }
    return static_cast<ULONG>(r);
  }

  STDMETHOD(CreateDevice)(REFGUID guid, LPDIRECTINPUTDEVICEA* out, LPUNKNOWN outer) override {
    const int slot = OurSlot(guid);
    if (slot < 0) return real_->CreateDevice(guid, out, outer);
    return CreatePad(slot, kIID_DevA, reinterpret_cast<void**>(out), outer);
  }

  STDMETHOD(EnumDevices)(DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags) override {
    if (!cb) return DIERR_INVALIDPARAM;
    const DWORD cls = GET_DIDEVICE_TYPE(type);
    const bool want_joy = cls == 0 || cls == DIDEVTYPE_JOYSTICK;
    const bool ff_only = (flags & DIEDFL_FORCEFEEDBACK) != 0;
    int listed = 0;
    if (!hid_scanned_) {
      hid_ = XInputHidDevices();
      hid_scanned_ = true;
    }
    if (want_joy && (!ff_only || RumbleOn())) {
      for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        XINPUT_STATE st;
        if (g_get_state(static_cast<DWORD>(slot), &st) != ERROR_SUCCESS) continue;
        {
          Lock lock;
          g_connected_mask |= 1u << slot;
        }
        NameSlot(slot);
        DIDEVICEINSTANCEA d;
        Pad::FillInstance(slot, &d);
        ++listed;
        patch::Log("xinput: EnumDevices: listing \"%s\" %s", d.tszInstanceName, GuidText(d.guidInstance).c_str());
        if (cb(&d, ref) == DIENUM_STOP) return DI_OK;
      }
      if (!listed) patch::Log("xinput: EnumDevices: no XInput controller connected (connect it before starting the game)");
    }
    Ctx ctx = {cb, ref, &hid_, false};
    return real_->EnumDevices(type, &Filter, &ctx, flags);
  }

  STDMETHOD(GetDeviceStatus)(REFGUID guid) override {
    const int slot = OurSlot(guid);
    if (slot < 0) return real_->GetDeviceStatus(guid);
    XINPUT_STATE st;
    return g_get_state(static_cast<DWORD>(slot), &st) == ERROR_SUCCESS ? DI_OK : DI_NOTATTACHED;
  }
  STDMETHOD(RunControlPanel)(HWND hwnd, DWORD flags) override { return real_->RunControlPanel(hwnd, flags); }
  STDMETHOD(Initialize)(HINSTANCE inst, DWORD version) override { return real_->Initialize(inst, version); }
  STDMETHOD(FindDevice)(REFGUID cls, LPCSTR name, LPGUID out) override { return real_->FindDevice(cls, name, out); }
  STDMETHOD(CreateDeviceEx)(REFGUID guid, REFIID iid, LPVOID* out, LPUNKNOWN outer) override {
    const int slot = OurSlot(guid);
    if (slot < 0) return real_->CreateDeviceEx(guid, iid, out, outer);
    return CreatePad(slot, iid, out, outer);
  }

 private:
  struct Ctx {
    LPDIENUMDEVICESCALLBACKA cb;
    LPVOID ref;
    const std::set<VidPid>* hid;
    bool unused;
  };

  static BOOL CALLBACK Filter(LPCDIDEVICEINSTANCEA d, LPVOID p) {
    auto* ctx = static_cast<Ctx*>(p);
    if (d && IsXInputCopy(d, *ctx->hid)) {
      patch::Log("xinput: EnumDevices: hiding Windows' DirectInput copy \"%s\" (the XInput controller is listed instead)",
                 d->tszInstanceName);
      return DIENUM_CONTINUE;
    }
    if (d) patch::Log("xinput: EnumDevices: passing \"%s\" (type 0x%lx)", d->tszInstanceName, d->dwDevType);
    return ctx->cb(d, ctx->ref);
  }

  static int OurSlot(REFGUID g) {
    for (int s = 0; s < XUSER_MAX_COUNT; ++s)
      if (Same(g, PadInstanceGuid(s))) return s;
    return -1;
  }

  HRESULT CreatePad(int slot, REFIID iid, void** out, LPUNKNOWN outer) {
    if (!out) return DIERR_INVALIDPARAM;
    *out = nullptr;
    if (outer) return DIERR_NOAGGREGATION;
    auto* pad = new Pad(slot);
    HRESULT hr = pad->QueryInterface(iid, out);
    pad->Release();
    patch::Log("xinput: created Xbox Controller %d (%s) hr=0x%08lx", slot + 1, GuidText(iid).c_str(),
               static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) StartThread();
    return hr;
  }

  IDirectInput7A* real_;
  volatile LONG refs_ = 1;
  std::set<VidPid> hid_;
  bool hid_scanned_ = false;
};

// ---------------------------------------------------------------- hook
using CreateExFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, LPUNKNOWN);
CreateExFn g_create = nullptr;

HRESULT WINAPI MyDirectInputCreateEx(HINSTANCE inst, DWORD version, REFIID iid, void** out, LPUNKNOWN outer) {
  HRESULT hr = g_create(inst, version, iid, out, outer);
  if (FAILED(hr) || !out || !*out) return hr;
  if (!Same(iid, kIID_DI7A)) {
    patch::Log("xinput: DirectInputCreateEx asked for %s, not wrapped", GuidText(iid).c_str());
    return hr;
  }
  if (!LoadXInput()) return hr;
  *out = static_cast<IDirectInput7A*>(new DirectInputWrap(static_cast<IDirectInput7A*>(*out)));
  patch::Log("xinput: DirectInput wrapped: XInput controllers are listed as joysticks");
  return hr;
}

}  // namespace

namespace xpad {

void Apply() {
  const Config& c = GetConfig();
  if (c.keep_control_preset) {
    // The game keeps the chosen control preset only while "use last
    // controller" is on (0x528760 sets PresetSelected); otherwise every start
    // resets it to the default for the device (0x44a1b0). Keep it whenever
    // the controller is the same one (0x528630 still compares the IDs).
    const unsigned char orig[] = {0x74, 0x40}, repl[] = {0x90, 0x90};
    patch::Replace("input: keep control preset", 0x5286de, orig, sizeof orig, repl, sizeof repl);
  }
  if (!c.xinput) {
    patch::Log("xinput: XInput=0, Xbox controllers stay as Windows' DirectInput devices");
    return;
  }
  InitializeCriticalSection(&g_cs);
  QueryPerformanceFrequency(&g_freq);
  g_log = c.xinput_log;
  g_rumble_pct = c.xinput_rumble;
  g_deadzone = static_cast<float>(c.xinput_deadzone) / 100.0f;
  g_curve = static_cast<float>(c.xinput_steer_curve) / 100.0f;
  g_rumble_forces = c.xinput_rumble_forces;
  g_kick_threshold = c.xinput_rumble_threshold * 100.0;
  g_steer_smooth_us = static_cast<float>(c.xinput_steer_smooth) * 1000.0f;
  g_trigger_buttons = c.xinput_trigger_buttons;
  constexpr uintptr_t kDirectInputCreateEx = 0x555028;  // DINPUT!DirectInputCreateEx
  if (!*reinterpret_cast<void**>(kDirectInputCreateEx)) {
    patch::Log("xinput: DirectInputCreateEx import not bound, XInput not installed");
    return;
  }
  g_create = reinterpret_cast<CreateExFn>(
      patch::HookIat(kDirectInputCreateEx, reinterpret_cast<void*>(&MyDirectInputCreateEx)));
  patch::Log("xinput: XInput=1 (XInputRumble=%d XInputRumbleForces=%d XInputRumbleThreshold=%d XInputDeadzone=%d XInputSteerCurve=%d XInputSteerSmooth=%d KeepControlPreset=%d "
             "XInputTriggerButtons=%d XInputLog=%d)",
             c.xinput_rumble, g_rumble_forces ? 1 : 0, c.xinput_rumble_threshold, c.xinput_deadzone, c.xinput_steer_curve, c.xinput_steer_smooth, c.keep_control_preset ? 1 : 0,
             g_trigger_buttons ? 1 : 0, g_log ? 1 : 0);
}

}  // namespace xpad
