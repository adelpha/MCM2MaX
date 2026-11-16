MCM2MaX menu screens (MenuMaX=1 in mcm2max.ini)
================================================

The screens in this folder are drawn by RmlUi (https://mikke89.github.io/RmlUiDoc/),
a library that lays out HTML/CSS-style text files. Open them in any text editor.

  settings.rml   the "More Settings" screen (Options > Graphics > More Settings)
  max.rcss       the look shared by every screen (colours, sizes, fonts)
  images\        pictures (PNG; pointer.png is the mouse pointer)
  fonts\         fonts (TTF/OTF, all loaded at start)

Live editing: while a screen is open in the game, saving any file in this folder
reloads it at once (F5 reloads too). Mistakes are written to the MCM2MaX log
(%LOCALAPPDATA%\Motocross Madness 2\logs\) as "MenuMaX: RmlUi ..." lines.

Sizes: "dp" is one pixel of the game's 640x480 menus, so 640dp x 480dp is the
menu area at any window size. Text and pictures are drawn at the window's real
resolution, so a picture can be bigger than its dp size for a sharper look.

Settings without code
---------------------
Any control can read and save an mcm2max.ini setting:

  <input type="checkbox" ini="MotionBlur" default="1"/>
  <input type="range" ini="ViewDistance" min="1" max="10" step="0.5" default="1" restart/>
  <select ini="Anisotropy" default="16"><option value="0">Off</option>...</select>

  ini="Key"           the setting it reads and saves
  default="v"         its value when mcm2max.ini doesn't have the key
  ini-on="v"          the value saved when a checkbox is ticked (default 1)
  restart             the change applies the next time the game starts
                      (shows the "* after restarting the game" note)
  game="Name"         a checkbox on the game's own page under the screen
                      (Options > Graphics: SkyCheckBox, ParticlesCheckBox,
                      ShadowsCheckBox)
  <span ini-text="Key" suffix="x" zero="Off"/>   shows a setting's value
  enable-if="Key!=0"  (or "Key=1") greys the control unless it holds
  autofocus           where the keyboard / controller starts

Saved values go to mcm2max.ini (or the per-user MCM2MaX-menu.ini if the game
folder can't be written), the same as the Enhancements box.

Buttons: <button onclick="back">Back</button>. Commands: back (or close),
open <screen> (another .rml in this folder), reload.

Mouse, keyboard and controllers
-------------------------------
Every screen can be used with all three, with no extra work:
  move between controls   arrow keys / Tab, d-pad / left stick
  press / tick / open     Enter or Space, controller A (button 1)
  change a slider / list  Left / Right
  back                    Esc, right mouse button, controller B (button 2)
Everything that can be picked needs "nav: auto" and "tab-index: auto" (set for
input, select and button in max.rcss); nav-up / nav-down / nav-left / nav-right
set a different order where the automatic one is wrong.
