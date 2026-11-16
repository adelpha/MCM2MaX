// Display modes and the monitor MCM2MaX uses; see display.h.
#include "display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "patch.h"

namespace display {
namespace {

bool g_init = false;
Mode g_desktop;
RECT g_rect{};
HMONITOR g_monitor = nullptr;
char g_device[32] = {};
std::vector<Mode> g_monitor_modes;  // every mode the monitor has (32-bit, progressive)
std::vector<Mode> g_game_modes;

struct FindMonitor {
  const char* device;
  HMONITOR found;
};

BOOL CALLBACK FindMonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM p) {
  auto* f = reinterpret_cast<FindMonitor*>(p);
  MONITORINFOEXA mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoA(mon, &mi) && !_stricmp(mi.szDevice, f->device)) {
    f->found = mon;
    return FALSE;
  }
  return TRUE;
}

// Monitor=1 is the primary display, 2 and up the other displays that are
// part of the desktop, in Windows' order.
bool PickDevice(int want, char* out, size_t size) {
  std::vector<std::string> others;
  std::string primary;
  DISPLAY_DEVICEA dd{};
  dd.cb = sizeof(dd);
  for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
    if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
      patch::Log("display: device %lu %s \"%s\"%s", i, dd.DeviceName, dd.DeviceString,
                 (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (primary)" : "");
      if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
        primary = dd.DeviceName;
      else
        others.push_back(dd.DeviceName);
    }
    dd.cb = sizeof(dd);
  }
  std::string pick = primary;
  if (want >= 2) {
    if (static_cast<size_t>(want - 2) < others.size())
      pick = others[want - 2];
    else
      patch::Log("display: Monitor=%d but only %u display(s); using the primary", want,
                 static_cast<unsigned>(others.size() + 1));
  }
  if (pick.empty()) return false;
  std::snprintf(out, size, "%s", pick.c_str());
  return true;
}

void BuildLists() {
  g_monitor_modes.clear();
  DEVMODEA dm{};
  dm.dmSize = sizeof(dm);
  for (DWORD i = 0; EnumDisplaySettingsA(g_device[0] ? g_device : nullptr, i, &dm); ++i) {
    const int w = static_cast<int>(dm.dmPelsWidth), h = static_cast<int>(dm.dmPelsHeight);
    const int hz = dm.dmDisplayFrequency > 1 ? static_cast<int>(dm.dmDisplayFrequency) : 0;
    const bool ok = dm.dmBitsPerPel == 32 && !(dm.dmDisplayFlags & DM_INTERLACED) && w >= 640 && h >= 480;
    if (ok) {
      bool dup = false;
      for (const Mode& m : g_monitor_modes) dup |= m.w == w && m.h == h && m.hz == hz;
      if (!dup) g_monitor_modes.push_back({w, h, hz});
    }
    dm.dmSize = sizeof(dm);
  }
  // Smallest first; for one size the highest refresh rate first.
  std::sort(g_monitor_modes.begin(), g_monitor_modes.end(), [](const Mode& a, const Mode& b) {
    if (a.w * a.h != b.w * b.h) return a.w * a.h < b.w * b.h;
    if (a.w != b.w) return a.w < b.w;
    return a.hz > b.hz;
  });

  g_game_modes.clear();
  if (GetConfig().exclusive) {
    g_game_modes = g_monitor_modes;
    if (g_game_modes.size() > 200) {
      patch::Log("display: %u exclusive modes, keeping the 200 largest", static_cast<unsigned>(g_game_modes.size()));
      g_game_modes.erase(g_game_modes.begin(), g_game_modes.end() - 200);
    }
  } else {
    for (const Mode& m : g_monitor_modes) {
      if (m.w > g_desktop.w || m.h > g_desktop.h) continue;
      bool dup = false;
      for (const Mode& g : g_game_modes) dup |= g.w == m.w && g.h == m.h;
      if (!dup) g_game_modes.push_back({m.w, m.h, 0});
    }
    bool has_desktop = false;
    for (const Mode& g : g_game_modes) has_desktop |= g.w == g_desktop.w && g.h == g_desktop.h;
    if (!has_desktop) g_game_modes.push_back({g_desktop.w, g_desktop.h, 0});
  }
  if (g_game_modes.empty()) g_game_modes.push_back({640, 480, 0});

  char line[2048];
  size_t len = 0;
  for (size_t i = 0; i < g_game_modes.size() && len + 24 < sizeof(line); ++i)
    len += std::snprintf(line + len, sizeof(line) - len, g_game_modes[i].hz ? " %dx%d@%d" : " %dx%d",
                         g_game_modes[i].w, g_game_modes[i].h, g_game_modes[i].hz);
  patch::Log("display: monitor has %u modes; the game is offered %u (%s):%s",
             static_cast<unsigned>(g_monitor_modes.size()), static_cast<unsigned>(g_game_modes.size()),
             GetConfig().exclusive ? "Exclusive: every size and refresh rate" : "Borderless: sizes up to the desktop",
             line);
}

}  // namespace

void Init() {
  if (g_init) return;
  g_init = true;
  const int want = GetConfig().monitor;
  if (!PickDevice(want, g_device, sizeof(g_device))) g_device[0] = 0;
  DEVMODEA dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsA(g_device[0] ? g_device : nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
    g_desktop = {static_cast<int>(dm.dmPelsWidth), static_cast<int>(dm.dmPelsHeight),
                 dm.dmDisplayFrequency > 1 ? static_cast<int>(dm.dmDisplayFrequency) : 0};
  } else {
    g_desktop = {GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), 0};
  }
  FindMonitor f{g_device, nullptr};
  if (g_device[0]) EnumDisplayMonitors(nullptr, nullptr, FindMonitorProc, reinterpret_cast<LPARAM>(&f));
  g_monitor = f.found;
  if (!g_monitor) g_monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoA(g_monitor, &mi);
  g_rect = mi.rcMonitor;
  patch::Log("display: Monitor=%d -> %s, desktop %dx%d@%d, at %ld,%ld %ldx%ld", want, g_device[0] ? g_device : "?",
             g_desktop.w, g_desktop.h, g_desktop.hz, g_rect.left, g_rect.top, g_rect.right - g_rect.left,
             g_rect.bottom - g_rect.top);
  BuildLists();
}

Mode Desktop() {
  Init();
  return g_desktop;
}

RECT MonitorRect() {
  Init();
  // The monitor can move or change size (exclusive mode switch): ask again.
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (g_monitor && GetMonitorInfoA(g_monitor, &mi)) g_rect = mi.rcMonitor;
  return g_rect;
}

HMONITOR Monitor() {
  Init();
  return g_monitor;
}

const char* DeviceName() {
  Init();
  return g_device;
}

const std::vector<Mode>& GameModes() {
  Init();
  return g_game_modes;
}

bool MonitorHas(int w, int h, int hz) {
  Init();
  for (const Mode& m : g_monitor_modes)
    if (m.w == w && m.h == h && (!hz || m.hz == hz)) return true;
  return false;
}

int BestHz(int w, int h) {
  Init();
  int best = 0;
  for (const Mode& m : g_monitor_modes)
    if (m.w == w && m.h == h && m.hz > best) best = m.hz;
  return best;
}

}  // namespace display
