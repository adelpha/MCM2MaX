// Resolution: mcm2max.ini decides the game's resolution, not the game.
//
// The game saves its resolution as a position in its list of display modes
// (options +0xfc4, the Options dropdown position) and starts at 640x480. Its
// list comes from EnumDisplayModes, i.e. from display.cpp (the monitor's own
// modes: sizes up to the desktop with Fullscreen=Borderless, every size and
// refresh rate with Fullscreen=Exclusive).
//
// The wanted mode is BorderlessResolution= or ExclusiveResolution= (each
// mode keeps its own), Desktop until the player picks one. Every frame the
// game's saved position is pointed at that mode; a pick in Options > Graphics
// is written back to the ini as its size (and refresh rate). A saved mode the
// monitor no longer has falls back to the desktop for that start; the ini is
// left as it is.
//
// The game shows its menus at 640x480 and switches to this mode for races;
// those switches stay virtual. With Fullscreen=Exclusive the screen is set to
// this mode the whole time (dd/backend*.cpp, ResolutionWanted), menus scaled.
//
// Exe (2008 no-CD Mcm2.exe):
//   [0x5743ac] = game; game+0xfc4 = saved mode position;
//   [game+0xc] = display card: +0x8 mode count, +0x10 mode array of
//   0x24-byte records {width, height, bpp, refresh, ...}, filled by the
//   EnumDisplayModes callback at 0x4cddb0.
//   Options > Graphics save (OptGraph exchange 0x4b6d80):
//     0x4b6da3 mov [0x68e918],eax, eax = the dropdown's position.
//   Another dialog's OK at 0x453846 does the same.
//   Options > Graphics fills the dropdown at 0x4b6b98: sprintf "%d X %d"
//   at 0x4b6bdb / 0x4b6c2c with esi = mode position.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "display.h"
#include "patch.h"

namespace {

int g_want_w = 0, g_want_h = 0, g_want_hz = 0;  // the mode the game should use (never 0 after Init)
bool g_logged_table = false;
int g_last_fixed = -2;  // last position written (for the log)
bool g_ready = false;

struct GameMode {
  int w, h, bpp, hz;
};

// The game's display mode list, or 0 entries while it doesn't exist yet.
int Modes(const uint8_t** arr) {
  const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x5743AC);
  if (!game) return 0;
  const uintptr_t dev = *reinterpret_cast<const uintptr_t*>(game + 0xC);
  if (!dev) return 0;
  const int n = *reinterpret_cast<const int*>(dev + 0x8);
  const uint8_t* a = *reinterpret_cast<const uint8_t* const*>(dev + 0x10);
  if (n <= 0 || n > 512 || !a) return 0;
  *arr = a;
  return n;
}

GameMode ModeAt(const uint8_t* arr, int i) {
  const int* m = reinterpret_cast<const int*>(arr + i * 0x24);
  return {m[0], m[1], m[2], m[3]};
}

// The ini's choice for the current Fullscreen mode, checked against what the
// monitor / list offers; the desktop otherwise.
void ChooseFromIni() {
  const Config& c = GetConfig();
  const display::Mode desk = display::Desktop();
  if (c.exclusive) {
    int w = c.fs_width, h = c.fs_height, hz = c.fs_hz;
    const char* why = "ExclusiveResolution";
    if (!w) {
      w = desk.w;
      h = desk.h;
      hz = desk.hz;
      why = "Desktop";
    } else if (!display::MonitorHas(w, h, 0)) {
      patch::Log("resolution: ExclusiveResolution=%dx%d@%d: the monitor has no such mode; desktop used this time", w, h,
                 hz);
      w = desk.w;
      h = desk.h;
      hz = desk.hz;
      why = "Desktop (fallback)";
    } else if (!hz || !display::MonitorHas(w, h, hz)) {
      const int best = display::BestHz(w, h);
      if (hz) patch::Log("resolution: %dx%d has no %d Hz mode; %d Hz used", w, h, hz, best);
      hz = best;
    }
    g_want_w = w;
    g_want_h = h;
    g_want_hz = hz;
    patch::Log("resolution: Fullscreen=Exclusive, mode %dx%d@%d (%s)", w, h, hz, why);
  } else {
    int w = c.bl_width, h = c.bl_height;
    const char* why = "BorderlessResolution";
    bool listed = false;
    for (const display::Mode& m : display::GameModes()) listed |= m.w == w && m.h == h;
    if (!w) {
      w = desk.w;
      h = desk.h;
      why = "Desktop";
    } else if (!listed) {
      patch::Log("resolution: BorderlessResolution=%dx%d is not available (desktop %dx%d); desktop used this time", w,
                 h, desk.w, desk.h);
      w = desk.w;
      h = desk.h;
      why = "Desktop (fallback)";
    }
    g_want_w = w;
    g_want_h = h;
    g_want_hz = 0;
    patch::Log("resolution: Fullscreen=Borderless, rendering at %dx%d (%s), scaled to %dx%d", w, h, why, desk.w, desk.h);
  }
}

// Options OK: the player picked the mode at this position.
void __stdcall NotePick(int pos) {
  const uint8_t* arr = nullptr;
  const int n = Modes(&arr);
  if (pos < 0 || pos >= n) {
    patch::Log("resolution: Options picked position %d, outside the %d modes; not saved", pos, n);
    return;
  }
  const GameMode m = ModeAt(arr, pos);
  const bool excl = GetConfig().exclusive;
  const int hz = excl ? m.hz : 0;
  if (m.w == g_want_w && m.h == g_want_h && hz == g_want_hz) return;
  g_want_w = m.w;
  g_want_h = m.h;
  g_want_hz = hz;
  char v[32];
  if (excl)
    std::snprintf(v, sizeof(v), "%dx%d@%d", m.w, m.h, m.hz);
  else
    std::snprintf(v, sizeof(v), "%dx%d", m.w, m.h);
  const char* key = excl ? "ExclusiveResolution" : "BorderlessResolution";
  const char* file = SetStringFromMenu(key, v);
  patch::Log("resolution: player picked %s (position %d); saved as %s=%s in %s%s", v, pos, key, v, file,
             excl ? "; the screen switches now (Direct3D 11) or at the next start (Direct3D 9)" : "");
}

// The dropdown's text for a mode: the game's "W X H", plus the refresh rate
// with Fullscreen=Exclusive. Called instead of the game's sprintf, with the
// mode position (esi) pushed in front by a stub.
int __cdecl LabelFormat(int pos, void* /*ret*/, char* buf, const char* /*fmt*/, int w, int h) {
  const uint8_t* arr = nullptr;
  const int n = Modes(&arr);
  int hz = 0;
  if (GetConfig().exclusive && pos >= 0 && pos < n) hz = ModeAt(arr, pos).hz;
  // The game's buffer is on its stack, 0x70 bytes; these stay far below that.
  if (hz > 0) return std::snprintf(buf, 48, "%d X %d  %d Hz", w, h, hz);
  return std::snprintf(buf, 48, "%d X %d", w, h);
}

// Replace "mov [0x68e918],eax" at site with a call to a stub that does the
// same and then reports the position to NotePick.
bool HookPickSite(uintptr_t site) {
  const uint8_t code[] = {0xA3, 0x18, 0xE9, 0x68, 0x00};  // mov [0068e918],eax
  if (!patch::Matches(site, code, sizeof(code))) {
    patch::Log("resolution: Options save code at %08x differs, not hooked", static_cast<unsigned>(site));
    return false;
  }
  auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return false;
  // mov [0x68e918],eax; pushad; push eax; call NotePick; popad; ret
  size_t n = 0;
  std::memcpy(t + n, code, sizeof(code));
  n += sizeof(code);
  t[n++] = 0x60;
  t[n++] = 0x50;
  t[n++] = 0xE8;
  const int32_t r = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&NotePick) -
                                         reinterpret_cast<uintptr_t>(t + n + 4));
  std::memcpy(t + n, &r, 4);
  n += 4;
  t[n++] = 0x61;
  t[n++] = 0xC3;
  FlushInstructionCache(GetCurrentProcess(), t, n);
  patch::Call(site, t, sizeof(code));
  return true;
}

// Replace "call sprintf" at site (dropdown label) with: push esi; call
// LabelFormat; add esp,4; ret.
uint8_t* g_label_stub = nullptr;
bool HookLabelSite(uintptr_t site) {
  const uint8_t code[] = {0xE8};
  if (!patch::Matches(site, code, 1) ||
      site + 5 + *reinterpret_cast<const int32_t*>(site + 1) != 0x53A11F) {
    patch::Log("resolution: dropdown label code at %08x differs, not hooked", static_cast<unsigned>(site));
    return false;
  }
  if (!g_label_stub) {
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return false;
    size_t n = 0;
    t[n++] = 0x56;  // push esi
    t[n++] = 0xE8;
    const int32_t r = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&LabelFormat) -
                                           reinterpret_cast<uintptr_t>(t + n + 4));
    std::memcpy(t + n, &r, 4);
    n += 4;
    t[n++] = 0x83;  // add esp,4
    t[n++] = 0xC4;
    t[n++] = 0x04;
    t[n++] = 0xC3;  // ret
    FlushInstructionCache(GetCurrentProcess(), t, n);
    g_label_stub = t;
  }
  patch::Call(site, g_label_stub, 5);
  return true;
}

}  // namespace

void ApplyResolutionDefault() {
  display::Init();
  MigrateDisplayKeys();
  const bool a = HookPickSite(0x4B6DA3);  // Options > Graphics
  const bool b = HookPickSite(0x453846);
  const bool l1 = HookLabelSite(0x4B6BDB);
  const bool l2 = HookLabelSite(0x4B6C2C);
  ChooseFromIni();
  g_ready = true;
  patch::Log("resolution: hooks: Options graphics %d, other %d, labels %d/%d", a, b, l1, l2);
}

void ResolutionWanted(int* w, int* h, int* hz) {
  if (!g_ready) {
    const display::Mode d = display::Desktop();
    *w = d.w;
    *h = d.h;
    *hz = d.hz;
    return;
  }
  *w = g_want_w;
  *h = g_want_h;
  *hz = g_want_hz;
}

// Called once a frame: point the game's saved position at the wanted mode.
void ResolutionFrame() {
  if (!g_ready) return;
  const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x5743AC);
  const uint8_t* arr = nullptr;
  const int n = Modes(&arr);
  if (!n) return;
  if (!g_logged_table) {
    g_logged_table = true;
    char line[4096];
    size_t len = 0;
    for (int i = 0; i < n && len + 40 < sizeof(line); ++i) {
      const GameMode m = ModeAt(arr, i);
      const int* r = reinterpret_cast<const int*>(arr + i * 0x24);
      len += std::snprintf(line + len, sizeof(line) - len, " %d:%dx%dx%d@%d/%d%d", i, m.w, m.h, m.bpp, m.hz, r[5],
                           r[6]);
    }
    patch::Log("resolution: the game's %d display modes (pos:WxHxbpp@Hz/usable flags):%s", n, line);
    const uintptr_t dev = *reinterpret_cast<const uintptr_t*>(game + 0xC);
    patch::Log("resolution: card: video memory %d MB, texture cache limit %d, flags70 %02x, partial blt %d, agp %d, "
               "disabled %02x",
               *reinterpret_cast<const int*>(dev + 0x54) >> 20, *reinterpret_cast<const int*>(dev + 0x60),
               *reinterpret_cast<const uint8_t*>(dev + 0x70), *reinterpret_cast<const int*>(dev + 0x5BC),
               *reinterpret_cast<const int*>(dev + 0x9F0), *reinterpret_cast<const uint8_t*>(dev + 0xB74));
  }
  int best = -1;
  for (int i = 0; i < n && best < 0; ++i) {
    const GameMode m = ModeAt(arr, i);
    if (m.w == g_want_w && m.h == g_want_h && (!g_want_hz || m.hz == g_want_hz)) best = i;
  }
  if (best < 0) {
    if (g_last_fixed != -1) {
      patch::Log("resolution: %dx%d@%d is not in the game's list; left at position %d", g_want_w, g_want_h, g_want_hz,
                 *reinterpret_cast<int*>(game + 0xFC4));
      g_last_fixed = -1;
    }
    return;
  }
  int& pos = *reinterpret_cast<int*>(game + 0xFC4);
  if (pos == best) return;
  if (g_last_fixed != best) {
    const GameMode was = pos >= 0 && pos < n ? ModeAt(arr, pos) : GameMode{0, 0, 0, 0};
    patch::Log("resolution: saved position %d (%dx%d@%d) -> %d (%dx%d@%d)", pos, was.w, was.h, was.hz, best,
               g_want_w, g_want_h, g_want_hz);
    g_last_fixed = best;
  }
  pos = best;
}
