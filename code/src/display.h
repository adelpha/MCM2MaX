// Display modes and the monitor MCM2MaX uses (display.cpp).
//
// The game's resolution list comes from here, not from the game's video
// card test: the real modes of the chosen monitor (Monitor=), as Windows
// lists them for that display. Fullscreen=Borderless offers each size once,
// up to the desktop size; Fullscreen=Exclusive offers every size and
// refresh rate the monitor has.
#pragma once
#include <windows.h>

#include <vector>

namespace display {

struct Mode {
  int w = 0, h = 0, hz = 0;  // hz 0 = any / not applicable (Borderless)
};

// Reads the monitor and its modes; called once at start (DllMain), before
// any mode change, so the desktop mode is the real one.
void Init();

// The monitor's desktop mode at start, and its area on the virtual screen.
Mode Desktop();
RECT MonitorRect();
HMONITOR Monitor();
const char* DeviceName();  // \\.\DISPLAYn

// The list the game is offered (EnumDisplayModes), smallest first.
const std::vector<Mode>& GameModes();

// True when the monitor has this exact mode (hz 0: any rate).
bool MonitorHas(int w, int h, int hz);
// The highest refresh rate the monitor has for this size, or 0.
int BestHz(int w, int h);

}  // namespace display
