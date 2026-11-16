// Resolution: start at the desktop resolution, and keep the player's
// choice by size instead of by list position, in mcm2max.ini.
//
// The game saves its resolution as a position in the list of display modes
// (options +0xfc4, the dropdown index), and starts at the first entry,
// 640x480, until the player picks one. Whenever the list changes (new
// MCM2MaX modes, another desktop size, a video card re-test) the saved
// position points at a different mode, e.g. 1600x1200 in place of
// 1920x1080 (StOo, 2026-11-16).
//
// So the size the player picks in Options is written to mcm2max.ini as
// Resolution=WxH (or the per-user menu ini when mcm2max.ini can't be
// written), and every frame the game's saved position is pointed at that
// size, or at the desktop while Resolution=Desktop. The game switches modes
// itself by comparing that position with the current one.
//
// Exe (2008 no-CD Mcm2.exe):
//   [0x5743ac] = game; game+0xfc4 = saved mode position;
//   [game+0xc] = display device: +0x8 mode count, +0x10 mode array of
//   0x24-byte records {width, height, bpp, ...} (compared at 0x4c5f53).
//   Options > Graphics save (OptGraph exchange 0x4b6d80):
//     0x4b6da3 mov [0x68e918],eax, eax = the dropdown's position.
//   Another dialog's OK at 0x453846 does the same.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "patch.h"

namespace {

int g_pick_w = 0, g_pick_h = 0;  // Resolution=WxH or the player's pick (0 = desktop)
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
    g_pick_w = m.w;
    g_pick_h = m.h;
    char v[32];
    std::snprintf(v, sizeof(v), "%dx%d", m.w, m.h);
    const char* file = SetStringFromMenu("Resolution", v);
    patch::Log("resolution: player picked %s (position %d); saved as Resolution=%s in %s", v, pos, v, file);
  }
}

}  // namespace

namespace {
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
}  // namespace

void ApplyResolutionDefault() {
  const Config& c = GetConfig();
  if (c.default_res_w < 0) {
    g_off = true;
    patch::Log("resolution: Resolution=Game, the game's own saved position is used");
    return;
  }
  const bool a = HookPickSite(0x4B6DA3);  // Options > Graphics
  const bool b = HookPickSite(0x453846);
  if (!a && !b) {
    g_off = true;
    patch::Log("resolution: no Options save code found; Resolution not applied");
    return;
  }
  g_pick_w = c.default_res_w;
  g_pick_h = c.default_res_h;
  if (g_pick_w)
    patch::Log("resolution: Resolution=%dx%d (Options hooks: graphics %d, other %d)", g_pick_w, g_pick_h, a, b);
  else
    patch::Log("resolution: Resolution=Desktop (%dx%d) (Options hooks: graphics %d, other %d)",
               GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), a, b);
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
  int w = g_pick_w, h = g_pick_h;
  const char* why = "Resolution";
  if (!w) {
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    why = "desktop";
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
