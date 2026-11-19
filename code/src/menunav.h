// MenuNav: the game's own menu screens (main menu, Options, ...) driven with
// the keyboard and controllers as well as the mouse, looking exactly as they
// do (see menunav.cpp).
#pragma once
#include <windows.h>

#include <cstdint>

// At start-up, after the config is loaded.
void MenuNavInstall();
// MenuNav=1 and installed (input_timing.cpp passes keys on and gives clicks).
bool MenuNavOn();
// A key went down / up (DirectInput key code), as the game read it (any thread).
void MenuNavKey(unsigned dik, bool down);
// The game's menu cursor is about to update (GUICursor, game thread, every
// menu frame; fixes_resolution.cpp's cursor hook). cursor = the GUICursor.
void MenuNavFrame(uintptr_t cursor);
// A click given to the game has finished (input_timing.cpp): how many of the
// game's buffered and immediate mouse reads saw it.
void MenuNavClickDone(unsigned data_reads, unsigned state_reads);
