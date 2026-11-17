// ControllerHub: every connected gamepad (through SDL3) feeds one game
// controller; the pad touched last drives it. xinput_pad.cpp presents the
// result to the game as a DirectInput joystick, as it does for XInput slots.
#pragma once
#include <windows.h>

#include <string>
#include <utility>
#include <vector>

namespace hub {

// One reading of the driving pad, in XInput's layout: sticks -32768..32767
// with up positive, triggers 0..255, XINPUT_GAMEPAD_* button bits (face
// buttons by position: A = bottom, B = right, X = left, Y = top).
struct PadState {
  SHORT lx, ly, rx, ry;
  BYTE lt, rt;
  WORD buttons;
};

// Button labels follow the driving pad's family.
enum Family { kXbox, kPlayStation, kNintendo, kGeneric };

// True when the DLL was built with SDL3 (-DMCM2MaX_SDL3).
bool Available();

// Starts SDL on the calling thread (the controller sampling thread) and finds
// the pads already connected. disabled: DisabledControllers.
bool Init(bool log, const char* disabled);

// Called by the sampling thread every tick: hot-plug, pad states, which pad
// drives, rumble.
void Pump();

// The driving pad's state; false (and a neutral state) when none is
// connected. packet changes whenever the state or the driving pad does.
bool Read(PadState* out, DWORD* packet);

// Motor speeds 0..65535 for the driving pad (sent by the next Pump).
void Rumble(WORD low, WORD high);

// USB vendor/product ids of every gamepad SDL sees (enabled or not), so the
// game's own DirectInput copies of them can be hidden.
std::vector<std::pair<WORD, WORD>> GamepadIds();

Family ActiveFamily();
std::string ActiveName();  // "" when none
int Count();               // gamepads connected (enabled ones)

// Writes a summary of every pad to the log.
void LogSummary();

}  // namespace hub
