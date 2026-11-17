// ControllerHub on SDL3: every connected gamepad feeds one game controller.
//
// SDL runs on the controller sampling thread (xinput_pad.cpp): Init and Pump
// are only ever called there. SDL finds Xbox pads (XInput / RawInput),
// PlayStation and Switch pads (HIDAPI) and other DirectInput gamepads it has a
// mapping for, and hands them over in one layout. Joysticks it can't map
// (wheels, flight sticks) are not opened: the game keeps seeing those as its
// own DirectInput devices.
//
// The pad that was used last drives: a button press, a trigger pulled past a
// quarter or a stick pushed past half way makes a pad the driving one. If the
// driving pad is unplugged, the most recently used remaining pad takes over.

#include "pad_hub.h"

#include <SDL3/SDL.h>
#include <xinput.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "patch.h"

namespace hub {
namespace {

struct Pad {
  SDL_JoystickID id = 0;
  SDL_Gamepad* g = nullptr;
  std::string name;
  WORD vid = 0, pid = 0;
  SDL_GamepadType type = SDL_GAMEPAD_TYPE_UNKNOWN;
  bool enabled = true;
  PadState st = {};
  Uint64 last_used = 0;  // SDL_GetTicksNS of the last press / push
  WORD rumble_low = 0, rumble_high = 0;
  Uint64 rumble_sent = 0;
  DWORD presses = 0;     // for the summary
};

CRITICAL_SECTION g_cs;
bool g_ready = false, g_log = false;
std::vector<Pad> g_pads;  // open gamepads, in the order SDL reported them
std::vector<std::pair<WORD, WORD>> g_all_ids;  // every gamepad's VID/PID
int g_active = -1;        // index into g_pads, or -1
DWORD g_packet = 0;
PadState g_out = {};      // the driving pad's last state, as handed out
WORD g_want_low = 0, g_want_high = 0;
std::vector<std::string> g_disabled;  // lower case

struct Lock {
  Lock() { EnterCriticalSection(&g_cs); }
  ~Lock() { LeaveCriticalSection(&g_cs); }
};

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void ParseDisabled(const char* text) {
  std::string cur;
  auto flush = [&] {
    size_t a = cur.find_first_not_of(" \t"), b = cur.find_last_not_of(" \t");
    if (a != std::string::npos) g_disabled.push_back(Lower(cur.substr(a, b - a + 1)));
    cur.clear();
  };
  for (const char* p = text; *p; ++p) {
    if (*p == ',' || *p == ';') flush();
    else cur += *p;
  }
  flush();
}

bool IsDisabled(const std::string& name, WORD vid, WORD pid) {
  char ids[16];
  std::snprintf(ids, sizeof ids, "%04x:%04x", vid, pid);
  const std::string n = Lower(name);
  for (const std::string& d : g_disabled) {
    if (d == ids) return true;
    if (d.size() >= 3 && n.find(d) != std::string::npos) return true;
  }
  return false;
}

const char* TypeName(SDL_GamepadType t) {
  switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One/Series";
    case SDL_GAMEPAD_TYPE_PS3: return "PS3";
    case SDL_GAMEPAD_TYPE_PS4: return "PS4";
    case SDL_GAMEPAD_TYPE_PS5: return "PS5";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Joy-Con (L)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Joy-Con (R)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con pair";
    case SDL_GAMEPAD_TYPE_STANDARD: return "standard";
    default: return "unknown";
  }
}

Family FamilyOf(SDL_GamepadType t) {
  switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return kXbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return kPlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return kNintendo;
    default: return kGeneric;
  }
}

SHORT Flip(Sint16 v) { return static_cast<SHORT>(v == -32768 ? 32767 : -v); }

PadState ReadPad(SDL_Gamepad* g) {
  PadState s = {};
  s.lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
  s.ly = Flip(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));  // SDL: down positive
  s.rx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
  s.ry = Flip(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
  s.lt = static_cast<BYTE>(std::max<int>(0, SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) * 255 / 32767);
  s.rt = static_cast<BYTE>(std::max<int>(0, SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) * 255 / 32767);
  static const struct {
    SDL_GamepadButton b;
    WORD bit;
  } kMap[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A},
      {SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B},
      {SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X},
      {SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK},
      {SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT},
  };
  for (const auto& m : kMap)
    if (SDL_GetGamepadButton(g, m.b)) s.buttons |= m.bit;
  return s;
}

bool Same(const PadState& a, const PadState& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

float Mag(SHORT x, SHORT y) {
  const float fx = x / 32767.0f, fy = y / 32767.0f;
  return fx * fx + fy * fy;
}

// A deliberate touch: a new button press, a trigger pulled past a quarter or
// a stick pushed past half way (resting sticks drift a little; that isn't one).
bool Touched(const PadState& was, const PadState& now) {
  if (now.buttons & ~was.buttons) return true;
  if ((now.lt > 64 && was.lt <= 64) || (now.rt > 64 && was.rt <= 64)) return true;
  if (Mag(now.lx, now.ly) > 0.25f && Mag(was.lx, was.ly) <= 0.25f) return true;
  if (Mag(now.rx, now.ry) > 0.25f && Mag(was.rx, was.ry) <= 0.25f) return true;
  return false;
}

void StopRumble(Pad& p) {
  if (p.rumble_low || p.rumble_high) SDL_RumbleGamepad(p.g, 0, 0, 0);
  p.rumble_low = p.rumble_high = 0;
}

void SetActive(int i, const char* why) {
  if (i == g_active) return;
  if (g_active >= 0 && g_active < static_cast<int>(g_pads.size())) StopRumble(g_pads[g_active]);
  g_active = i;
  ++g_packet;
  if (i >= 0)
    patch::Log("hub: %s now drives (%s)", g_pads[i].name.c_str(), why);
  else
    patch::Log("hub: no controller drives (%s)", why);
}

// Most recently used enabled pad, or the first enabled one, or -1.
int Best() {
  int best = -1;
  for (int i = 0; i < static_cast<int>(g_pads.size()); ++i) {
    if (!g_pads[i].enabled) continue;
    if (best < 0 || g_pads[i].last_used > g_pads[best].last_used) best = i;
  }
  return best;
}

void RefreshIds() {
  g_all_ids.clear();
  int n = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&n);
  for (int i = 0; ids && i < n; ++i)
    g_all_ids.emplace_back(SDL_GetGamepadVendorForID(ids[i]), SDL_GetGamepadProductForID(ids[i]));
  SDL_free(ids);
}

void Add(SDL_JoystickID id) {
  for (const Pad& p : g_pads)
    if (p.id == id) return;
  const SDL_JoystickType jt = SDL_GetJoystickTypeForID(id);
  const char* jname = SDL_GetGamepadNameForID(id);
  if (jt != SDL_JOYSTICK_TYPE_GAMEPAD && jt != SDL_JOYSTICK_TYPE_UNKNOWN) {
    patch::Log("hub: \"%s\" is not a gamepad (joystick type %d): left to the game as a DirectInput device",
               jname ? jname : "?", static_cast<int>(jt));
    return;
  }
  SDL_Gamepad* g = SDL_OpenGamepad(id);
  if (!g) {
    patch::Log("hub: could not open \"%s\": %s", jname ? jname : "?", SDL_GetError());
    return;
  }
  Pad p;
  p.id = id;
  p.g = g;
  const char* name = SDL_GetGamepadName(g);
  p.name = name && *name ? name : "Gamepad";
  p.vid = SDL_GetGamepadVendor(g);
  p.pid = SDL_GetGamepadProduct(g);
  p.type = SDL_GetGamepadType(g);
  p.enabled = !IsDisabled(p.name, p.vid, p.pid);
  p.st = ReadPad(g);
  const char* path = SDL_GetGamepadPath(g);
  patch::Log("hub: connected \"%s\" (%s, VID_%04X PID_%04X, %s%s)%s path %s", p.name.c_str(), TypeName(p.type), p.vid,
             p.pid, SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "rumble" : "no rumble",
             SDL_GetGamepadConnectionState(g) == SDL_JOYSTICK_CONNECTION_WIRELESS ? ", wireless" : "",
             p.enabled ? "" : " DISABLED by DisabledControllers", path ? path : "?");
  g_pads.push_back(p);
  if (g_active < 0 && p.enabled) SetActive(static_cast<int>(g_pads.size()) - 1, "connected");
}

void Remove(SDL_JoystickID id) {
  for (int i = 0; i < static_cast<int>(g_pads.size()); ++i) {
    if (g_pads[i].id != id) continue;
    patch::Log("hub: disconnected \"%s\"", g_pads[i].name.c_str());
    SDL_CloseGamepad(g_pads[i].g);
    const bool was_active = i == g_active;
    g_pads.erase(g_pads.begin() + i);
    if (g_active > i) --g_active;
    if (was_active) {
      g_active = -1;
      SetActive(Best(), "the driving controller was unplugged");
      if (g_active < 0) {
        g_out = PadState{};
        ++g_packet;
      }
    }
    return;
  }
}

}  // namespace

bool Available() { return true; }

bool Init(bool log, const char* disabled) {
  InitializeCriticalSection(&g_cs);
  g_log = log;
  ParseDisabled(disabled ? disabled : "");
  // No SDL window exists, so SDL never thinks the game is in the background;
  // the hint makes sure. The joystick thread watches for hot-plugging.
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
  if (!SDL_Init(SDL_INIT_GAMEPAD)) {
    patch::Log("hub: SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  int v = SDL_GetVersion();
  patch::Log("hub: SDL %d.%d.%d (%s), joystick drivers ready", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
             SDL_VERSIONNUM_MICRO(v), SDL_GetRevision());
  // Extra mappings: gamecontrollerdb.txt next to Mcm2.exe (optional).
  char db[MAX_PATH] = {};
  if (GetModuleFileNameA(nullptr, db, MAX_PATH)) {
    if (char* slash = std::strrchr(db, '\\')) {
      std::snprintf(slash + 1, static_cast<size_t>(db + MAX_PATH - slash - 1), "gamecontrollerdb.txt");
      if (GetFileAttributesA(db) != INVALID_FILE_ATTRIBUTES) {
        const int n = SDL_AddGamepadMappingsFromFile(db);
        patch::Log("hub: %s: %d mappings added%s%s", db, n, n < 0 ? ": " : "", n < 0 ? SDL_GetError() : "");
      }
    }
  }
  if (!g_disabled.empty()) {
    std::string list;
    for (const std::string& d : g_disabled) list += (list.empty() ? "" : ", ") + d;
    patch::Log("hub: DisabledControllers: %s", list.c_str());
  }
  Lock lock;
  int n = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&n);
  for (int i = 0; ids && i < n; ++i) Add(ids[i]);
  SDL_free(ids);
  RefreshIds();
  patch::Log("hub: %d gamepad(s) at start%s", static_cast<int>(g_pads.size()),
             g_pads.empty() ? " (plugging one in later works)" : "");
  g_ready = true;
  return true;
}

void Pump() {
  if (!g_ready) return;
  SDL_PumpEvents();
  SDL_Event e;
  Lock lock;
  bool changed_list = false;
  while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
      Add(e.gdevice.which);
      changed_list = true;
    } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
      Remove(e.gdevice.which);
      changed_list = true;
    }
  }
  if (changed_list) RefreshIds();
  const Uint64 now = SDL_GetTicksNS();
  for (int i = 0; i < static_cast<int>(g_pads.size()); ++i) {
    Pad& p = g_pads[i];
    const PadState s = ReadPad(p.g);
    if (Touched(p.st, s)) {
      p.last_used = now;
      ++p.presses;
      if (p.enabled && i != g_active) SetActive(i, "it was used");
    }
    p.st = s;
  }
  if (g_active >= 0) {
    Pad& a = g_pads[g_active];
    if (!Same(a.st, g_out)) {
      g_out = a.st;
      ++g_packet;
    }
    // Rumble: resend while running so SDL's duration never runs out.
    const bool change = a.rumble_low != g_want_low || a.rumble_high != g_want_high;
    const bool running = g_want_low || g_want_high;
    if (change || (running && now - a.rumble_sent > 500000000ull)) {
      SDL_RumbleGamepad(a.g, g_want_low, g_want_high, running ? 1000 : 0);
      a.rumble_low = g_want_low;
      a.rumble_high = g_want_high;
      a.rumble_sent = now;
    }
  }
}

bool Read(PadState* out, DWORD* packet) {
  if (!g_ready) {
    *out = PadState{};
    *packet = 0;
    return false;
  }
  Lock lock;
  *out = g_active >= 0 ? g_out : PadState{};
  *packet = g_packet;
  return g_active >= 0;
}

void Rumble(WORD low, WORD high) {
  if (!g_ready) return;
  Lock lock;
  g_want_low = low;
  g_want_high = high;
}

std::vector<std::pair<WORD, WORD>> GamepadIds() {
  if (!g_ready) return {};
  Lock lock;
  return g_all_ids;
}

Family ActiveFamily() {
  if (!g_ready) return kXbox;
  Lock lock;
  return g_active >= 0 ? FamilyOf(g_pads[g_active].type) : kXbox;
}

std::string ActiveName() {
  if (!g_ready) return "";
  Lock lock;
  return g_active >= 0 ? g_pads[g_active].name : "";
}

int Count() {
  if (!g_ready) return 0;
  Lock lock;
  int n = 0;
  for (const Pad& p : g_pads) n += p.enabled ? 1 : 0;
  return n;
}

void LogSummary() {
  if (!g_ready) return;
  Lock lock;
  for (int i = 0; i < static_cast<int>(g_pads.size()); ++i) {
    const Pad& p = g_pads[i];
    patch::Log("hub: %s\"%s\" %s: lx %d ly %d rx %d ry %d lt %u rt %u buttons 0x%04x, %lu touches, rumble %u/%u",
               i == g_active ? "* " : "  ", p.name.c_str(), p.enabled ? "enabled" : "disabled", p.st.lx, p.st.ly,
               p.st.rx, p.st.ry, p.st.lt, p.st.rt, p.st.buttons, p.presses, p.rumble_low, p.rumble_high);
  }
}

}  // namespace hub
