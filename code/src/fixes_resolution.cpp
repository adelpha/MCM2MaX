// DefaultResolution: start at the desktop resolution, and keep the player's
// choice by size instead of by list position.
//
// The game saves its resolution as a position in the list of display modes
// (options +0xfc4, the dropdown index), and starts at the first entry,
// 640x480, until the player picks one. Whenever the list changes (new
// MCM2MaX modes, another desktop size, a video card re-test) the saved
// position points at a different mode, e.g. 1600x1200 in place of
// 1920x1080 (StOo, 2026-11-16).
//
// So the size the player picks in Options is stored as "WxH" in
// HKCU\Software\MCM2MaX\Motocross Madness 2\Resolution, and every frame the
// game's saved position is pointed at that size, or at DefaultResolution
// (the desktop by default) while the player has never picked one. The game
// switches modes itself by comparing that position with the current one.
//
// Exe (2008 no-CD Mcm2.exe):
//   [0x5743ac] = game; game+0xfc4 = saved mode position;
//   [game+0xc] = display device: +0x8 mode count, +0x10 mode array of
//   0x24-byte records {width, height, bpp, ...} (compared at 0x4c5f53).
//   0x453846 mov [0x68e918],eax: Options OK, eax = the dropdown's position.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "patch.h"

namespace {

const char kKey[] = "Software\\MCM2MaX\\Motocross Madness 2";
const char kValue[] = "Resolution";

int g_pick_w = 0, g_pick_h = 0;  // the player's choice (0 = none yet)
bool g_logged_table = false;
int g_last_fixed = -2;           // last position written (for the log)
bool g_off = false;

struct Mode {
  int w, h, bpp;
};

// The game's display mode list, or 0 entries while it doesn't exist yet.
int Modes(const uint8_t** arr) {
  const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x5743AC);
  if (!game) return 0;
  const uintptr_t dev = *reinterpret_cast<const uintptr_t*>(game + 0xC);
  if (!dev) return 0;
  const int n = *reinterpret_cast<const int*>(dev + 0x8);
  const uint8_t* a = *reinterpret_cast<const uint8_t* const*>(dev + 0x10);
  if (n <= 0 || n > 256 || !a) return 0;
  *arr = a;
  return n;
}

Mode ModeAt(const uint8_t* arr, int i) {
  const int* m = reinterpret_cast<const int*>(arr + i * 0x24);
  return {m[0], m[1], m[2]};
}

void LoadPick() {
  HKEY key;
  if (RegOpenKeyExA(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
  char v[32] = {};
  DWORD size = sizeof(v) - 1, type = 0;
  if (RegQueryValueExA(key, kValue, nullptr, &type, reinterpret_cast<BYTE*>(v), &size) == ERROR_SUCCESS &&
      type == REG_SZ) {
    int w = 0, h = 0;
    if (std::sscanf(v, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      g_pick_w = w;
      g_pick_h = h;
    }
  }
  RegCloseKey(key);
}

void SavePick(int w, int h) {
  HKEY key;
  if (RegCreateKeyExA(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
      ERROR_SUCCESS) {
    patch::Log("resolution: cannot open HKCU\\%s to save %dx%d", kKey, w, h);
    return;
  }
  char v[32];
  std::snprintf(v, sizeof(v), "%dx%d", w, h);
  RegSetValueExA(key, kValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(v), static_cast<DWORD>(std::strlen(v) + 1));
  RegCloseKey(key);
}

// Options OK: the player picked the mode at this position.
void __stdcall NotePick(int pos) {
  const uint8_t* arr = nullptr;
  const int n = Modes(&arr);
  if (pos < 0 || pos >= n) {
    patch::Log("resolution: Options picked position %d, outside the %d modes; not saved", pos, n);
    return;
  }
  const Mode m = ModeAt(arr, pos);
  if (m.w != g_pick_w || m.h != g_pick_h) {
    patch::Log("resolution: player picked %dx%d (position %d); saved", m.w, m.h, pos);
    g_pick_w = m.w;
    g_pick_h = m.h;
    SavePick(m.w, m.h);
  }
}

}  // namespace

void ApplyResolutionDefault() {
  const Config& c = GetConfig();
  if (c.default_res_w < 0) {
    g_off = true;
    patch::Log("resolution: DefaultResolution=Game, the game's own saved position is used");
    return;
  }
  const uint8_t site[] = {0xA3, 0x18, 0xE9, 0x68, 0x00};  // 00453846 mov [0068e918],eax
  if (!patch::Matches(0x453846, site, sizeof(site))) {
    g_off = true;
    patch::Log("resolution: Options code differs, DefaultResolution not applied");
    return;
  }
  auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return;
  // mov [0x68e918],eax; pushad; push eax; call NotePick; popad; ret
  size_t n = 0;
  std::memcpy(t + n, site, sizeof(site));
  n += sizeof(site);
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
  patch::Call(0x453846, t, sizeof(site));
  LoadPick();
  if (g_pick_w)
    patch::Log("resolution: player's saved choice %dx%d", g_pick_w, g_pick_h);
  else if (c.default_res_w > 0)
    patch::Log("resolution: no saved choice yet; DefaultResolution %dx%d", c.default_res_w, c.default_res_h);
  else
    patch::Log("resolution: no saved choice yet; DefaultResolution=Desktop (%dx%d)",
               GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

// Called once a frame: point the game's saved position at the wanted size.
void ResolutionFrame() {
  if (g_off) return;
  const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x5743AC);
  const uint8_t* arr = nullptr;
  const int n = Modes(&arr);
  if (!n) return;
  if (!g_logged_table) {
    g_logged_table = true;
    char line[1024];
    size_t len = 0;
    for (int i = 0; i < n && len + 32 < sizeof(line); ++i) {
      const Mode m = ModeAt(arr, i);
      len += std::snprintf(line + len, sizeof(line) - len, " %d:%dx%dx%d", i, m.w, m.h, m.bpp);
    }
    patch::Log("resolution: the game's %d display modes:%s", n, line);
  }
  const Config& c = GetConfig();
  int w = g_pick_w, h = g_pick_h;
  const char* why = "player's choice";
  if (!w) {
    w = c.default_res_w > 0 ? c.default_res_w : GetSystemMetrics(SM_CXSCREEN);
    h = c.default_res_h > 0 ? c.default_res_h : GetSystemMetrics(SM_CYSCREEN);
    why = c.default_res_w > 0 ? "DefaultResolution" : "desktop";
  }
  // Exact size, else the largest mode that fits inside it.
  int best = -1;
  for (int i = 0; i < n; ++i) {
    const Mode m = ModeAt(arr, i);
    if (m.w == w && m.h == h) {
      best = i;
      break;
    }
    if (m.w <= w && m.h <= h &&
        (best < 0 || m.w * m.h > ModeAt(arr, best).w * ModeAt(arr, best).h))
      best = i;
  }
  if (best < 0) return;
  int& pos = *reinterpret_cast<int*>(game + 0xFC4);
  if (pos == best) return;
  if (g_last_fixed != best) {
    const Mode was = pos >= 0 && pos < n ? ModeAt(arr, pos) : Mode{0, 0, 0};
    const Mode now = ModeAt(arr, best);
    patch::Log("resolution: saved position %d (%dx%d) -> %d (%dx%d, %s %dx%d)", pos, was.w, was.h, best, now.w,
               now.h, why, w, h);
    g_last_fixed = best;
  }
  pos = best;
}
