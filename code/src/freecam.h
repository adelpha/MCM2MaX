#pragma once
#include <cstdint>

// FreeCam (Ctrl+F6): freeze the race and fly the camera. See freecam.cpp.
bool FreeCamActive();
// Called every tick before drawing; true while flying (the race must not be
// simulated this tick).
bool FreeCamFrame(bool racing, double dt);
// From the camera update hook (0x42fd50), before the game builds the view.
void __stdcall FreeCamApply(uintptr_t cam);
