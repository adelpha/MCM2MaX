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
bool g_want_desktop = true;  // ...because the ini says Desktop (the dropdown shows "Desktop")
// The dropdown's "Desktop" entry: item data the game's mode positions never reach.
constexpr int kDesktopData = 1000;
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
    g_want_desktop = why[0] == 'D';
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
    g_want_desktop = why[0] == 'D';
    patch::Log("resolution: Fullscreen=Borderless, rendering at %dx%d (%s), scaled to %dx%d", w, h, why, desk.w, desk.h);
  }
}

// Options OK: the player picked the mode at this position (or "Desktop").
void __stdcall NotePick(int pos) {
  const uint8_t* arr = nullptr;
  const int n = Modes(&arr);
  const bool excl = GetConfig().exclusive;
  if (pos == kDesktopData) {
    const display::Mode d = display::Desktop();
    int real = -1;
    for (int i = 0; i < n && real < 0; ++i) {
      const GameMode m = ModeAt(arr, i);
      if (m.w == d.w && m.h == d.h && (!excl || m.hz == d.hz)) real = i;
    }
    for (int i = 0; i < n && real < 0; ++i) {
      const GameMode m = ModeAt(arr, i);
      if (m.w == d.w && m.h == d.h) real = i;
    }
    // The game keeps a mode position: give it the desktop's.
    *reinterpret_cast<int*>(0x68E918) = real >= 0 ? real : 0;
    const char* key = excl ? "ExclusiveResolution" : "BorderlessResolution";
    if (!g_want_desktop) {
      const char* file = SetStringFromMenu(key, "Desktop");
      patch::Log("resolution: player picked Desktop (%dx%d@%d, position %d); saved as %s=Desktop in %s", d.w, d.h,
                 excl ? d.hz : 0, real, key, file);
    }
    g_want_w = d.w;
    g_want_h = d.h;
    g_want_hz = excl ? d.hz : 0;
    g_want_desktop = true;
    return;
  }
  if (pos < 0 || pos >= n) {
    patch::Log("resolution: Options picked position %d, outside the %d modes; not saved", pos, n);
    return;
  }
  const GameMode m = ModeAt(arr, pos);
  const int hz = excl ? m.hz : 0;
  if (m.w == g_want_w && m.h == g_want_h && hz == g_want_hz && !g_want_desktop) return;
  g_want_desktop = false;
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
// with Fullscreen=Exclusive. Called instead of the game's sprintf by a stub
// that pushes copies of the arguments (never the game's own stack slots: a
// cdecl callee may reuse its argument slots, and res-1 crashed when a tail
// call overwrote the game's return address that way).
int __cdecl LabelFormat(int pos, char* buf, int w, int h) {
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

// Replace "call sprintf(buf, fmt, w, h)" at site (dropdown label) with a
// stub: push h; push w; push buf; push esi; call LabelFormat; add esp,16; ret.
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
    // On entry: [esp] ret, +4 buf, +8 fmt, +12 w, +16 h.
    const uint8_t head[] = {
        0xFF, 0x74, 0x24, 0x10,  // push [esp+16]  h
        0xFF, 0x74, 0x24, 0x10,  // push [esp+16]  w
        0xFF, 0x74, 0x24, 0x0C,  // push [esp+12]  buf
        0x56,                    // push esi       mode position
        0xE8};                   // call LabelFormat
    size_t n = sizeof(head);
    std::memcpy(t, head, n);
    const int32_t r = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&LabelFormat) -
                                           reinterpret_cast<uintptr_t>(t + n + 4));
    std::memcpy(t + n, &r, 4);
    n += 4;
    t[n++] = 0x83;  // add esp,16
    t[n++] = 0xC4;
    t[n++] = 0x10;
    t[n++] = 0xC3;  // ret
    FlushInstructionCache(GetCurrentProcess(), t, n);
    g_label_stub = t;
  }
  patch::Call(site, g_label_stub, 5);
  return true;
}

// ---- The game's video card test (CardTest=0: replaced) -----------------
// At start the game loads each card's cached test results from the registry
// (DriverInfo) or tests the card (0x4c5460: a 640x480 mode switch, video
// memory, AGP, texture formats, a partial blit), after writing the card's
// identity to the registry (0x4c5e00). With MCM2MaX's renderer the answers
// never change, so both are skipped and the card gets the values the test
// itself gave on Direct3D 11 (res-1 log, 2026-11-16).
constexpr uintptr_t kCards = 0x690894, kCardCount = 0x6908A4;

int __fastcall CardSetup(uintptr_t /*game*/) {
  const int n = *reinterpret_cast<const int*>(kCardCount);
  for (int i = 0; i < n && i < 8; ++i) {
    const uintptr_t card = reinterpret_cast<const uintptr_t*>(kCards)[i];
    if (!card) continue;
    *reinterpret_cast<int*>(card + 0x54) = 256 << 20;      // TotalVideoMemory (GetAvailableVidMem)
    *reinterpret_cast<int*>(card + 0x60) = 0x7FFFFFFF;     // TextureCacheLimit: none
    *reinterpret_cast<int*>(card + 0x5BC) = 0;             // PartialTextureBlt
    *reinterpret_cast<int*>(card + 0x9F0) = 1;             // IsAGP
    const int modes = *reinterpret_cast<const int*>(card + 0x8);
    uint8_t* arr = *reinterpret_cast<uint8_t**>(card + 0x10);
    for (int m = 0; arr && m < modes && m < 512; ++m) {  // every mode usable
      reinterpret_cast<int*>(arr + m * 0x24)[5] = 1;
      reinterpret_cast<int*>(arr + m * 0x24)[6] = 1;
    }
    patch::Log("resolution: card %d \"%s\": the game's card test skipped (CardTest=0); %d modes, video memory 256 MB, "
               "flags70 %02x, disabled %02x",
               i, reinterpret_cast<const char*>(card + 0x4BC), modes, *reinterpret_cast<const uint8_t*>(card + 0x70),
               *reinterpret_cast<const uint8_t*>(card + 0xB74));
  }
  return 1;
}

int __fastcall SkipCardIdentity(uintptr_t) { return 1; }

void ReplaceCardTest() {
  const uint8_t id_call[] = {0xE8};
  const bool a = patch::Matches(0x4C4716, id_call, 1) &&
                 0x4C4716 + 5 + *reinterpret_cast<const int32_t*>(0x4C4717) == 0x4C5E00;
  const bool b = patch::Matches(0x4C471D, id_call, 1) &&
                 0x4C471D + 5 + *reinterpret_cast<const int32_t*>(0x4C471E) == 0x4C5460;
  if (!a || !b) {
    patch::Log("resolution: card test code differs (%d/%d); the game's own test runs", a, b);
    return;
  }
  patch::Call(0x4C4716, reinterpret_cast<void*>(&SkipCardIdentity), 5);
  patch::Call(0x4C471D, reinterpret_cast<void*>(&CardSetup), 5);
  patch::Log("resolution: the game's video card test is replaced (CardTest=1 runs it)");
}

// ---- Borderless / Exclusive switched in Options --------------------------
// The game's mode table is refilled through its own EnumDisplayModes callback
// (0x4cddb0, stdcall(desc, card)), which grows the array as needed.
constexpr uintptr_t kEnumCallback = 0x4CDDB0;
using EnumCbFn = HRESULT(__stdcall*)(void* desc, void* ctx);

void RefillGameTable() {
  const uintptr_t game = *reinterpret_cast<const uintptr_t*>(0x5743AC);
  if (!game) return;
  const uintptr_t card = *reinterpret_cast<const uintptr_t*>(game + 0xC);
  if (!card) return;
  const uint8_t cb_start[] = {0x56, 0x8B, 0x74, 0x24, 0x0C};  // push esi; mov esi,[esp+0c]
  if (!patch::Matches(kEnumCallback, cb_start, sizeof(cb_start))) {
    patch::Log("resolution: mode list code differs; the list changes at the next start");
    return;
  }
  const int before = *reinterpret_cast<const int*>(card + 0x8);
  *reinterpret_cast<int*>(card + 0x8) = 0;
  for (const display::Mode& m : display::GameModes()) {
    uint8_t desc[0x7C] = {};
    *reinterpret_cast<uint32_t*>(desc + 0x00) = 0x7C;
    *reinterpret_cast<uint32_t*>(desc + 0x08) = m.h;
    *reinterpret_cast<uint32_t*>(desc + 0x0C) = m.w;
    *reinterpret_cast<uint32_t*>(desc + 0x18) = m.hz ? m.hz : 60;
    *reinterpret_cast<uint32_t*>(desc + 0x54) = 16;
    reinterpret_cast<EnumCbFn>(kEnumCallback)(desc, reinterpret_cast<void*>(card));
  }
  g_logged_table = false;  // log the new table next frame
  g_last_fixed = -2;
  patch::Log("resolution: the game's mode list refilled: %d -> %d modes", before,
             *reinterpret_cast<const int*>(card + 0x8));
}

// The resolution dropdown (list object at control +0x1fc): items 0x38 bytes
// at +0x214, count +0x1ec, selection +0x1f4; text (item +0x14) allocated
// with the game's allocator 0x4a4410, freed with 0x4a4450.
using AddItemFn = int(__fastcall*)(uintptr_t list, void*, const char* text, int data, int flags);
using SelectFn = int(__fastcall*)(uintptr_t list, void*, int data);
using FreeFn = void(__cdecl*)(void* p, const char* file, int line);

void RefillDropdown(uintptr_t control) {
  if (!control || IsBadReadPtr(reinterpret_cast<void*>(control), 0x200)) return;
  const uintptr_t list = *reinterpret_cast<const uintptr_t*>(control + 0x1FC);
  if (!list || IsBadReadPtr(reinterpret_cast<void*>(list), 0x218)) {
    patch::Log("resolution: dropdown list not found");
    return;
  }
  const int count = *reinterpret_cast<const int*>(list + 0x1EC);
  uint8_t* items = *reinterpret_cast<uint8_t**>(list + 0x214);
  for (int i = 0; items && i < count; ++i) {
    void*& text = *reinterpret_cast<void**>(items + i * 0x38 + 0x14);
    if (text) reinterpret_cast<FreeFn>(0x4A4450)(text, "MCM2MaX", 0);
    text = nullptr;
  }
  *reinterpret_cast<int*>(list + 0x1EC) = 0;
  *reinterpret_cast<int*>(list + 0x1F4) = -1;
  const uint8_t* arr = nullptr;
  const int n = Modes(&arr);
  // "Desktop" first: picking it writes Desktop to the ini (NotePick).
  reinterpret_cast<AddItemFn>(0x477D10)(list, nullptr, "Desktop", kDesktopData, 0);
  int select = -1;
  for (int i = 0; i < n; ++i) {
    const GameMode m = ModeAt(arr, i);
    char label[48];
    LabelFormat(i, label, m.w, m.h);
    reinterpret_cast<AddItemFn>(0x477D10)(list, nullptr, label, i, 0);
    if (select < 0 && m.w == g_want_w && m.h == g_want_h && (!g_want_hz || m.hz == g_want_hz)) select = i;
  }
  if (select < 0) select = 0;
  // The page's working copy of the mode position stays a real position.
  *reinterpret_cast<int*>(0x68E918) = select;
  reinterpret_cast<SelectFn>(0x477AC0)(list, nullptr, g_want_desktop ? kDesktopData : select);
  patch::Log("resolution: dropdown filled: %d -> %d entries + Desktop, showing %s (position %d)", count, n,
             g_want_desktop ? "Desktop" : "the mode", select);
}

}  // namespace

void ApplyResolutionDefault() {
  display::Init();
  MigrateDisplayKeys();
  if (!GetConfig().card_test) ReplaceCardTest();
  else patch::Log("resolution: CardTest=1, the game's own video card test runs");
  const bool a = HookPickSite(0x4B6DA3);  // Options > Graphics
  const bool b = HookPickSite(0x453846);
  const bool l1 = HookLabelSite(0x4B6BDB);
  const bool l2 = HookLabelSite(0x4B6C2C);
  ChooseFromIni();
  g_ready = true;
  patch::Log("resolution: hooks: Options graphics %d, other %d, labels %d/%d", a, b, l1, l2);
}

void ResolutionFillDropdown(uintptr_t dropdown) {
  if (g_ready) RefillDropdown(dropdown);
}

void ResolutionSetExclusive(bool exclusive, uintptr_t dropdown) {
  if (exclusive == GetConfig().exclusive) return;
  const DWORD t0 = GetTickCount();
  const char* file = SetFullscreenFromMenu(exclusive);
  const DWORD t1 = GetTickCount();
  patch::Log("resolution: Fullscreen=%s from Options, saved to %s (%lu ms)", exclusive ? "Exclusive" : "Borderless", file,
             static_cast<unsigned long>(t1 - t0));
  display::Rebuild();
  ChooseFromIni();
  RefillGameTable();
  RefillDropdown(dropdown);
  patch::Log("resolution: switch done in %lu ms (lists %lu ms)", static_cast<unsigned long>(GetTickCount() - t0),
             static_cast<unsigned long>(GetTickCount() - t1));
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
