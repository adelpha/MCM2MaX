// "Keep this display mode?" after an Exclusive mode pick (fixes_resolution.cpp;
// drawn by dd/racetitle.cpp, keys and clicks from input_timing.cpp).
#pragma once

#include <windows.h>

// Seconds left (10..0), or -1 when no prompt is up.
int ModePromptSeconds();
bool ModePromptActive();
constexpr int kPromptKeep = 1, kPromptRevert = 2, kPromptClick = 3;
// A key or click while the prompt is up (any thread); clicks are matched
// against the buttons at the menu cursor.
void ModePromptAnswer(int answer, const char* how);
// Where the prompt's buttons were drawn (surface pixels).
void ModePromptSetButtons(const RECT& keep, const RECT& revert, UINT surf_w, UINT surf_h);
// The menu cursor's position (surface pixels), if seen.
bool ModePromptCursor(POINT* p);
