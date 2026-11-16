// Race title intro (ini RaceTitle=1): during the countdown, the event's name
// and lap count (stunts: time limit) are shown in the middle of the screen,
// opening out of a white line and closing back into it as the countdown
// ends, instead of the game's small green text in the bottom-left corner.
#pragma once

#include <windows.h>

namespace dd {

class Surface;

void InstallRaceTitleHooks();
// A frame was shown: the title is drawn again at the next end of scene.
void RaceTitleNewFrame();
// The game ended a scene on `rt`: draw the title over it (once per frame).
void RaceTitleDraw(Surface* rt);
// A short debug message (debug keys), shown at the top of the screen for
// three seconds (GPU text only). Any thread.
void DebugNotice(const char* text);
// The "Keep this display mode?" prompt, if one is up, over `rt` (the
// frame about to be shown); area = where it was drawn (to erase after).
bool ModePromptDraw(Surface* rt, RECT* area);

}  // namespace dd
