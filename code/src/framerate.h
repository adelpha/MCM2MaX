// Frame pacing: frame-rate cap and precise frame time (see framerate.cpp).
#pragma once
#include <cstdint>

// Patch the game's frame timer and set up the cap. Call once at startup.
void ApplyFramePacing();

// Block until the frame cap (FrameCap in mcm2max.ini) allows another frame.
// Call without holding the renderer lock.
void WaitForFrameSlot();

// While races run at a fixed simulation rate: the number of the last finished
// simulation step and how far (0..1) the frame being drawn is past it.
// Returns false when drawing should use the game's values as they are.
bool SimDrawPoint(unsigned* tick, float* alpha);

// True while races run at the fixed simulation rate.
bool SimRacing();

// A race (BikeRace) has set its countdown / is being destroyed (race title
// hooks, dd/racetitle.cpp). MenuFpsCap leaves the frame rate alone between.
void RaceAlive(uintptr_t race);
void RaceGone(uintptr_t race);
