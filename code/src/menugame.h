// MenuMaX: the game's own menu controls, found by name on the screens shown
// now, read and changed for a MenuMaX screen drawn over them (ui/ui.cpp,
// game="Name"). Reads happen at once (game thread); changes are queued and
// made at the start of the game's next tick (outside the menu code, where a
// click may close or replace a page), the way the game's own clicks go.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mg {

enum Kind { kUnknown, kButton, kCheck, kRadio, kDropdown, kEdit, kList, kSlider, kScrollArrow, kStatic };

struct Info {
  Kind kind = kUnknown;
  bool shown = false;    // shown and taking input (the game's own test)
  bool enabled = false;  // not greyed out
  int value = 0;         // checkbox / radio / multi-state: state; slider: position; list / dropdown: chosen row (-1 none)
  int range = 0;         // multi-state: number of states; slider: highest position; list / dropdown: rows
  std::string text;      // the control's text (labels, a multi-state's state text, an edit box's text)
  RECT rect{};           // in 640x480 menu pixels
  uintptr_t dialog = 0;
};

// Looks at the screens shown now (game thread, at most once a frame).
void Refresh();
// The control called `name` on a shown screen (the topmost modal one first).
bool Get(const char* name, Info* out);
// A list's or dropdown's rows.
bool Rows(const char* name, std::vector<std::string>* rows);
// A modal dialog is shown now: its controls' names.
bool Modal(std::vector<std::string>* names);

// Changes, made at the start of the next tick.
void Click(const char* name);                   // button, checkbox, radio, multi-state, tab
void SetSlider(const char* name, int position, int event);  // event: 15 grab, 3 change, 16 release (as dragging sends)
void Choose(const char* name, int row);         // list / dropdown row, as clicking it does
void SetText(const char* name, const std::string& text);    // edit box

// framerate.cpp, at the start of the game's tick (game thread).
void Tick();
// fixes_resolution.cpp's menu cursor hook (game thread, every menu frame):
// changes are made here when no tick has come for a while.
void Frame();

}  // namespace mg
