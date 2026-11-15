// XInput controllers (Xbox pads) shown to the game as DirectInput joysticks.
#pragma once

namespace xpad {

// Hooks the game's DirectInputCreateEx import. Call before input::Apply so
// InputTiming wraps the devices made here like any other joystick.
void Apply();

}  // namespace xpad
