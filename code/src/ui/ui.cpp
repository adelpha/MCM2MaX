// MenuMaX: see ui.h.
#include "ui.h"

#include <RmlUi/Core.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../config.h"
#include "../pad_hub.h"
#include "../mode_prompt.h"
#include "../patch.h"
#include "../dd/backend.h"
#include "render_d3d11.h"
#include "render_d3d9.h"

// menus.cpp: the game's own controls on the open Options > Graphics page.
bool MenusGameGet(const char* name, int* value);
bool MenusGameSet(const char* name, int value);
void MenusRefreshFromIni();

namespace ui {

namespace {

// ---------------------------------------------------------------- helpers

std::wstring Widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}
std::string Narrow(const std::wstring& w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
  return s;
}

std::wstring GameDir() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring s(buf);
  const size_t slash = s.find_last_of(L'\\');
  return slash == std::wstring::npos ? std::wstring(L".\\") : s.substr(0, slash + 1);
}
std::wstring UiDir() { return GameDir() + L"UI\\MaX\\"; }

// "1.500000" -> "1.5"; non-numbers unchanged.
std::string Tidy(const std::string& v) {
  char* end = nullptr;
  const double d = std::strtod(v.c_str(), &end);
  if (v.empty() || !end || *end) return v;
  char b[32];
  std::snprintf(b, sizeof(b), "%g", d);
  return b;
}
bool IsZero(const std::string& v) {
  if (v.empty()) return true;
  char* end = nullptr;
  const double d = std::strtod(v.c_str(), &end);
  if (end && !*end) return d == 0;
  return _stricmp(v.c_str(), "off") == 0 || _stricmp(v.c_str(), "false") == 0;
}

// ---------------------------------------------------------------- RmlUi interfaces

class System : public Rml::SystemInterface {
 public:
  System() {
    QueryPerformanceFrequency(&freq_);
    QueryPerformanceCounter(&t0_);
  }
  double GetElapsedTime() override {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - t0_.QuadPart) / static_cast<double>(freq_.QuadPart);
  }
  bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
    const char* what = type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT ? "error"
                       : type == Rml::Log::LT_WARNING                            ? "warning"
                       : type == Rml::Log::LT_INFO                               ? "info"
                                                                                 : "debug";
    if (type == Rml::Log::LT_DEBUG) return true;
    if (++logged_ <= 300) patch::Log("MenuMaX: RmlUi %s: %s", what, message.c_str());
    if (logged_ == 300) patch::Log("MenuMaX: RmlUi: no more messages are logged this run");
    return true;
  }

 private:
  LARGE_INTEGER freq_{}, t0_{};
  unsigned logged_ = 0;
};

// Files by UTF-8 path, so a game folder with any name works.
class Files : public Rml::FileInterface {
 public:
  Rml::FileHandle Open(const Rml::String& path) override {
    FILE* f = _wfopen(Widen(path).c_str(), L"rb");
    if (!f && ++misses_ <= 50) patch::Log("MenuMaX: file not found: %s", path.c_str());
    return reinterpret_cast<Rml::FileHandle>(f);
  }
  void Close(Rml::FileHandle file) override { std::fclose(reinterpret_cast<FILE*>(file)); }
  size_t Read(void* buffer, size_t size, Rml::FileHandle file) override {
    return std::fread(buffer, 1, size, reinterpret_cast<FILE*>(file));
  }
  bool Seek(Rml::FileHandle file, long offset, int origin) override {
    return std::fseek(reinterpret_cast<FILE*>(file), offset, origin) == 0;
  }
  size_t Tell(Rml::FileHandle file) override { return static_cast<size_t>(std::ftell(reinterpret_cast<FILE*>(file))); }

 private:
  unsigned misses_ = 0;
};

// ---------------------------------------------------------------- state

struct Setting {
  Rml::Element* el = nullptr;
  std::string key;       // ini key (or game control name)
  bool game = false;     // a control on the game's own page, not an ini key
  bool checkbox = false;
  std::string on_value;  // written when a checkbox is ticked
  std::string value;     // as shown now
};

struct State {
  std::recursive_mutex mu;  // input queue and open/close requests
  bool installed = false;
  bool failed = false;   // RmlUi could not start: never again this run
  bool inited = false;
  System* system = nullptr;
  Files* files = nullptr;
  Renderer* render = nullptr;  // r11 or r9, by the renderer running
  RenderD3D11* r11 = nullptr;
  RenderD3D9* r9 = nullptr;
  Rml::Context* context = nullptr;
  Rml::ElementDocument* doc = nullptr;
  std::string want_open;  // set by Open, done at the next present
  std::string open_name;  // the screen shown
  bool want_close = false;
  bool active = false;    // input is taken (set as soon as Open is asked)
  // Live reload.
  unsigned long long signature = 0;
  DWORD last_watch = 0;
  // Settings on the open screen.
  std::vector<Setting> settings;
  std::map<std::string, std::string> pending;  // key -> value not yet saved
  DWORD last_change = 0;
  bool binding = false;  // values being put into controls: not changes
  // Input.
  struct Key {
    Rml::Input::KeyIdentifier id;
    bool down;
  };
  std::vector<Key> keys;
  std::vector<std::pair<int, bool>> buttons;
  int wheel = 0;
  DWORD joy_pov = 0xFFFFFFFF, joy_buttons = 0;
  bool joy_seen = false;
  // Held direction (keys, d-pad, stick) repeats.
  Rml::Input::KeyIdentifier held = Rml::Input::KI_UNKNOWN;
  DWORD held_since = 0, held_last = 0;
  WORD pad_prev[8] = {};
  bool pads_from_hub = false;
  size_t pad_count = 0, stick_count = 0;
  DWORD stick_prev[8] = {};
  int pad_dir_prev = -1;
  // One press seen through two devices at once (a keyboard that is also an
  // XInput pad, the game reading the keyboard two ways) counts once.
  bool key_down[256] = {};
  Rml::Input::KeyIdentifier last_key = Rml::Input::KI_UNKNOWN;
  int last_key_source = -1;
  DWORD last_key_at = 0;
  unsigned dup_keys = 0;
  // The control that had focus last: a click on empty space or text keeps it.
  Rml::Element* last_focus = nullptr;
  bool shift = false;
  // Pointer.
  // The screen's own pointer (window pixels), moved by the mouse movement
  // kept from the game (its cursor stays where it was, so the page under the
  // screen does not react to it).
  bool pointer_set = false;
  float px = 0, py = 0;
  int dx = 0, dy = 0;  // movement not yet applied (DirectInput units)
  unsigned moves = 0;
  Rml::TextureHandle pointer_tex = 0;
  Rml::Vector2i pointer_dims;
  bool pointer_tried = false;
  int mouse_x = -1, mouse_y = -1;
  // Statistics (logged every few seconds while open).
  unsigned frames = 0, key_events = 0, clicks = 0, saves = 0, reloads = 0;
  DWORD last_report = 0, opened_at = 0;
};
State g;

void Report(bool force);
void FlushPending(const char* why);

// ---------------------------------------------------------------- XInput

struct XPad {
  DWORD packet;
  WORD buttons;
  BYTE lt, rt;
  SHORT lx, ly, rx, ry;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XPad*);
XInputGetStateFn LoadXInput() {
  static XInputGetStateFn fn = [] {
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
      if (HMODULE m = LoadLibraryA(dll)) {
        if (auto f = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")))) {
          patch::Log("MenuMaX: controllers through %s", dll);
          return f;
        }
      }
    }
    patch::Log("MenuMaX: no XInput DLL; controllers only through DirectInput");
    return static_cast<XInputGetStateFn>(nullptr);
  }();
  return fn;
}

// ---------------------------------------------------------------- settings

void Collect(Rml::Element* e, std::vector<Rml::Element*>& out) {
  if (e->HasAttribute("ini") || e->HasAttribute("game") || e->HasAttribute("ini-text") || e->HasAttribute("enable-if"))
    out.push_back(e);
  for (int i = 0; i < e->GetNumChildren(); ++i) Collect(e->GetChild(i), out);
}

std::string CurrentValue(const std::string& key, const std::string& def) {
  auto it = g.pending.find(key);
  if (it != g.pending.end()) return it->second;
  std::string v;
  if (ReadSetting(key.c_str(), &v) && !v.empty()) return Tidy(v);
  return def;
}

void SetControl(Setting& s, const std::string& v) {
  s.value = v;
  if (s.checkbox) {
    if (IsZero(v)) s.el->RemoveAttribute("checked");
    else s.el->SetAttribute("checked", "");
  } else if (auto* fc = dynamic_cast<Rml::ElementFormControl*>(s.el)) {
    fc->SetValue(v);
  }
}

// <span ini-text="Key" zero="Off" suffix="x"> shows a setting's value.
void UpdateTexts() {
  std::vector<Rml::Element*> els;
  if (!g.doc) return;
  Collect(g.doc, els);
  for (Rml::Element* e : els) {
    if (e->HasAttribute("ini-text")) {
      const std::string key = e->GetAttribute<Rml::String>("ini-text", "");
      const std::string v = CurrentValue(key, e->GetAttribute<Rml::String>("default", ""));
      std::string text = IsZero(v) && e->HasAttribute("zero") ? e->GetAttribute<Rml::String>("zero", "")
                                                              : v + e->GetAttribute<Rml::String>("suffix", "");
      if (e->GetInnerRML() != text) e->SetInnerRML(text);
    }
    // enable-if="Key!=0" / "Key=1": greyed (disabled) otherwise.
    if (e->HasAttribute("enable-if")) {
      const std::string cond = e->GetAttribute<Rml::String>("enable-if", "");
      const size_t ne = cond.find("!="), eq = cond.find('=');
      bool on = true;
      if (ne != std::string::npos) on = Tidy(CurrentValue(cond.substr(0, ne), "0")) != Tidy(cond.substr(ne + 2));
      else if (eq != std::string::npos) on = Tidy(CurrentValue(cond.substr(0, eq), "0")) == Tidy(cond.substr(eq + 1));
      const bool was = !e->HasAttribute("disabled");
      if (on != was) {
        if (on) e->RemoveAttribute("disabled");
        else e->SetAttribute("disabled", "");
      }
    }
  }
}

void Bind() {
  g.settings.clear();
  std::vector<Rml::Element*> els;
  Collect(g.doc, els);
  g.binding = true;
  for (Rml::Element* e : els) {
    const bool game = e->HasAttribute("game");
    if (!game && !e->HasAttribute("ini")) continue;
    Setting s;
    s.el = e;
    s.game = game;
    s.key = e->GetAttribute<Rml::String>(game ? "game" : "ini", "");
    s.checkbox = e->GetTagName() == "input" && e->GetAttribute<Rml::String>("type", "") == "checkbox";
    std::string v;
    if (game) {
      int iv = 0;
      if (!MenusGameGet(s.key.c_str(), &iv)) {
        patch::Log("MenuMaX: game control %s not found on the page under the screen; greyed", s.key.c_str());
        e->SetAttribute("disabled", "");
      }
      v = std::to_string(iv);
    } else {
      v = CurrentValue(s.key, e->GetAttribute<Rml::String>("default", "0"));
    }
    s.on_value = !IsZero(v) && s.checkbox ? v : e->GetAttribute<Rml::String>("ini-on", "1");
    SetControl(s, v);
    patch::Log("MenuMaX: %s %s = %s (%s%s)", game ? "game control" : "setting", s.key.c_str(), v.c_str(),
               e->GetTagName().c_str(), s.checkbox ? " checkbox" : "");
    g.settings.push_back(s);
  }
  g.binding = false;
  UpdateTexts();
  patch::Log("MenuMaX: %u controls bound on %s", static_cast<unsigned>(g.settings.size()), g.open_name.c_str());
}

void Save(const std::string& key, const std::string& v) {
  const char* file;
  char* end = nullptr;
  const long iv = std::strtol(v.c_str(), &end, 10);
  // Whole numbers through SetFromMenu: the settings that can change at once
  // (VSync, MotionBlur, AlphaToCoverage, Anisotropy) do.
  if (!v.empty() && end && !*end && key != "ViewDistance") file = SetFromMenu(key.c_str(), static_cast<int>(iv));
  else file = SetStringFromMenu(key.c_str(), v.c_str());
  ++g.saves;
  patch::Log("MenuMaX: %s=%s saved to %s", key.c_str(), v.c_str(), file);
}

void FlushPending(const char* why) {
  if (g.pending.empty()) return;
  patch::Log("MenuMaX: saving %u setting(s) (%s)", static_cast<unsigned>(g.pending.size()), why);
  for (const auto& kv : g.pending) Save(kv.first, kv.second);
  g.pending.clear();
}

void OnChange(Rml::Element* target) {
  if (g.binding) return;
  for (Setting& s : g.settings) {
    if (s.el != target) continue;
    std::string v;
    if (s.checkbox) v = target->HasAttribute("checked") ? s.on_value : "0";
    else if (auto* fc = dynamic_cast<Rml::ElementFormControl*>(target)) v = Tidy(fc->GetValue());
    if (v == s.value) return;
    patch::Log("MenuMaX: %s %s: %s -> %s", s.game ? "game control" : "setting", s.key.c_str(), s.value.c_str(), v.c_str());
    s.value = v;
    if (s.checkbox && !IsZero(v)) s.on_value = v;
    if (s.game) {
      if (!MenusGameSet(s.key.c_str(), IsZero(v) ? 0 : std::atoi(v.c_str())))
        patch::Log("MenuMaX: game control %s could not be set", s.key.c_str());
    } else {
      g.pending[s.key] = v;  // saved once it stops changing (sliders send many)
      g.last_change = GetTickCount();
    }
    if (target->HasAttribute("restart") && g.doc) {
      if (Rml::Element* body = g.doc) body->SetClass("restart-needed", true);
    }
    UpdateTexts();
    return;
  }
}

// ---------------------------------------------------------------- events

class ChangeListener : public Rml::EventListener {
 public:
  void ProcessEvent(Rml::Event& e) override { OnChange(e.GetTargetElement()); }
};
ChangeListener g_change;

// onclick="close" / "open <screen>" / "reload": what a button does.
class Command : public Rml::EventListener {
 public:
  explicit Command(const Rml::String& c) : cmd_(c) {}
  void ProcessEvent(Rml::Event& e) override {
    ++g.clicks;
    patch::Log("MenuMaX: %s on %s#%s: %s", e.GetType().c_str(), e.GetCurrentElement()->GetTagName().c_str(),
               e.GetCurrentElement()->GetId().c_str(), cmd_.c_str());
    if (cmd_ == "close" || cmd_ == "back") {
      Close("button");
    } else if (cmd_.compare(0, 5, "open ") == 0) {
      Open(cmd_.substr(5).c_str(), "button");
    } else if (cmd_ == "reload") {
      g.signature = 0;  // reloads at the next look
      g.last_watch = 0;
    } else {
      patch::Log("MenuMaX: unknown command \"%s\" (close, back, open <screen>, reload)", cmd_.c_str());
    }
  }
  void OnDetach(Rml::Element*) override { delete this; }

 private:
  std::string cmd_;
};
class Commands : public Rml::EventListenerInstancer {
 public:
  Rml::EventListener* InstanceEventListener(const Rml::String& value, Rml::Element*) override { return new Command(value); }
};
Commands g_commands;

// ---------------------------------------------------------------- files

void Signature(const std::wstring& dir, unsigned long long& sig, int depth) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.cFileName[0] == L'.') continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (depth < 4) Signature(dir + fd.cFileName + L"\\", sig, depth + 1);
      continue;
    }
    unsigned long long t = (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                           fd.ftLastWriteTime.dwLowDateTime;
    sig = sig * 1099511628211ull ^ (t + fd.nFileSizeLow);
    for (const wchar_t* c = fd.cFileName; *c; ++c) sig = sig * 31 + *c;
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

void LoadFonts() {
  int n = 0;
  auto load = [&](const std::wstring& path, bool fallback) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    const bool ok = Rml::LoadFontFace(Narrow(path), fallback);
    patch::Log("MenuMaX: font %ls %s%s", path.c_str(), ok ? "loaded" : "FAILED", fallback ? " (fallback)" : "");
    n += ok;
  };
  // UI\MaX\fonts\ first (the screens' own fonts), then the HUD fonts, then
  // Windows' own (on every Windows since XP) so text always shows.
  for (const wchar_t* pat : {L"*.ttf", L"*.otf"}) {
    WIN32_FIND_DATAW fd;
    const std::wstring dir = UiDir() + L"fonts\\";
    HANDLE h = FindFirstFileW((dir + pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do load(dir + fd.cFileName, false);
    while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  load(GameDir() + L"FONTS\\ContrailOne-Regular.ttf", false);
  wchar_t win[MAX_PATH] = {};
  GetWindowsDirectoryW(win, MAX_PATH);
  const std::wstring fonts = std::wstring(win) + L"\\Fonts\\";
  load(fonts + L"arial.ttf", true);
  load(fonts + L"arialbd.ttf", false);
  load(fonts + L"tahoma.ttf", false);
  load(fonts + L"tahomabd.ttf", false);
  patch::Log("MenuMaX: %d font faces", n);
}

bool InitRml() {
  if (g.inited) return true;
  if (g.failed) return false;
  const DWORD t0 = GetTickCount();
  g.system = new System;
  g.files = new Files;
  if (dd::IsD3D9()) {
    g.r9 = new RenderD3D9;
    g.render = g.r9;
    patch::Log("MenuMaX: drawing with Direct3D 9 (fixed function)");
  } else {
    g.r11 = new RenderD3D11;
    g.render = g.r11;
    if (!g.r11->Init(dd::B().device)) {
      g.failed = true;
      return false;
    }
  }
  g.render->SetBaseDir(UiDir());
  Rml::SetSystemInterface(g.system);
  Rml::SetFileInterface(g.files);
  Rml::SetRenderInterface(g.render);
  if (!Rml::Initialise()) {
    patch::Log("MenuMaX: RmlUi could not start");
    g.failed = true;
    return false;
  }
  Rml::Factory::RegisterEventListenerInstancer(&g_commands);
  LoadFonts();
  g.context = Rml::CreateContext("menu", Rml::Vector2i(640, 480));
  if (!g.context) {
    patch::Log("MenuMaX: RmlUi context could not be made");
    g.failed = true;
    return false;
  }
  g.inited = true;
  patch::Log("MenuMaX: RmlUi %s started in %lu ms; screens from %ls", Rml::GetVersion().c_str(),
             static_cast<unsigned long>(GetTickCount() - t0), UiDir().c_str());
  return true;
}

void CloseDoc() {
  if (!g.doc) return;
  FlushPending("screen closed");
  g.doc->RemoveEventListener(Rml::EventId::Change, &g_change);
  g.doc->Close();
  g.doc = nullptr;
  g.last_focus = nullptr;
  g.settings.clear();
  g.context->Update();  // documents go away at the next update
}

bool LoadDoc(const std::string& name, const std::string& focus_id) {
  CloseDoc();
  const DWORD t0 = GetTickCount();
  const std::string path = Narrow(UiDir()) + name + ".rml";
  g.doc = g.context->LoadDocument(path);
  if (!g.doc) {
    patch::Log("MenuMaX: %s could not be loaded (see the RmlUi messages above)", path.c_str());
    return false;
  }
  g.doc->AddEventListener(Rml::EventId::Change, &g_change);
  g.open_name = name;
  Bind();
  g.doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::Auto);
  Rml::Element* f = focus_id.empty() ? nullptr : g.doc->GetElementById(focus_id);
  if (!f) {
    Rml::ElementList list;
    g.doc->QuerySelectorAll(list, "[autofocus]");
    if (!list.empty()) f = list[0];
  }
  if (f) f->Focus(true);
  g.signature = 0;
  Signature(UiDir(), g.signature, 0);
  g.last_watch = GetTickCount();
  patch::Log("MenuMaX: %s loaded in %lu ms (focus on %s)", path.c_str(), static_cast<unsigned long>(GetTickCount() - t0),
             f ? f->GetId().c_str() : "nothing");
  return true;
}

void Watch() {
  const DWORD now = GetTickCount();
  if (now - g.last_watch < 500) return;
  g.last_watch = now;
  unsigned long long sig = 0;
  Signature(UiDir(), sig, 0);
  if (sig == g.signature) return;
  const std::string focus = g.context->GetFocusElement() ? g.context->GetFocusElement()->GetId() : std::string();
  patch::Log("MenuMaX: files in UI\\MaX changed; reloading %s", g.open_name.c_str());
  ++g.reloads;
  FlushPending("reload");
  Rml::Factory::ClearStyleSheetCache();
  Rml::Factory::ClearTemplateCache();
  CloseDoc();
  Rml::ReleaseTextures();
  if (!LoadDoc(g.open_name, focus)) {
    g.signature = sig;  // wait for the next change
    patch::Log("MenuMaX: the screen stays closed until its files load again (Esc / B leaves)");
  }
}

// ---------------------------------------------------------------- input

Rml::Input::KeyIdentifier FromDik(unsigned dik) {
  using namespace Rml::Input;
  switch (dik) {
    case 0x01: return KI_ESCAPE;
    case 0x0E: return KI_BACK;
    case 0x0F: return KI_TAB;
    case 0x1C: return KI_RETURN;
    case 0x9C: return KI_NUMPADENTER;
    case 0x39: return KI_SPACE;
    case 0xC8: case 0x48: return KI_UP;
    case 0xD0: case 0x50: return KI_DOWN;
    case 0xCB: case 0x4B: return KI_LEFT;
    case 0xCD: case 0x4D: return KI_RIGHT;
    case 0xC9: return KI_PRIOR;
    case 0xD1: return KI_NEXT;
    case 0xC7: return KI_HOME;
    case 0xCF: return KI_END;
    case 0xD3: return KI_DELETE;
    case 0x3F: return KI_F5;
    case 0x2A: return KI_LSHIFT;
    case 0x36: return KI_RSHIFT;
    default: return KI_UNKNOWN;
  }
}

bool Repeats(Rml::Input::KeyIdentifier k) {
  using namespace Rml::Input;
  return k == KI_UP || k == KI_DOWN || k == KI_LEFT || k == KI_RIGHT || k == KI_TAB;
}

bool IsControl(Rml::Element* e) {
  if (!e) return false;
  const Rml::String& t = e->GetTagName();
  return t == "input" || t == "select" || t == "button";
}

// After any input: remember the focused control, or give focus back to the
// last one when a click on empty space or text took it away.
void KeepFocus(const char* why) {
  if (!g.context || !g.doc) return;
  Rml::Element* f = g.context->GetFocusElement();
  if (IsControl(f)) {
    g.last_focus = f;
    return;
  }
  if (g.last_focus) {
    g.last_focus->Focus();
    if (GetConfig().menu_log) patch::Log("MenuMaX: focus kept on %s#%s (%s)", g.last_focus->GetTagName().c_str(),
                                         g.last_focus->GetId().c_str(), why);
  }
}

// Where a press came from: 0 keyboard, 1 controller, 2 other.
int SourceOf(const char* from) {
  if (std::strcmp(from, "keyboard") == 0) return 0;
  if (std::strncmp(from, "controller", 10) == 0 || std::strncmp(from, "joystick", 8) == 0) return 1;
  return 2;
}

void Press(Rml::Input::KeyIdentifier k, const char* from) {
  using namespace Rml::Input;
  {
    const int src = SourceOf(from);
    const DWORD now = GetTickCount();
    if (src < 2 && k == g.last_key && g.last_key_source >= 0 && g.last_key_source != src && now - g.last_key_at < 150) {
      ++g.dup_keys;
      if (GetConfig().menu_log || g.dup_keys <= 10)
        patch::Log("MenuMaX: key %d from %s ignored: the same press came from the %s %lu ms before", static_cast<int>(k),
                   from, g.last_key_source == 0 ? "keyboard" : "controller", static_cast<unsigned long>(now - g.last_key_at));
      g.last_key_at = now;
      return;
    }
    if (src < 2) {
      g.last_key = k;
      g.last_key_source = src;
      g.last_key_at = now;
    }
  }
  ++g.key_events;
  auto* select = g.context ? dynamic_cast<Rml::ElementFormControlSelect*>(g.context->GetFocusElement()) : nullptr;
  if (select && select->IsSelectBoxVisible() && (k == KI_ESCAPE || k == KI_BACK)) {
    select->CancelSelectBox();  // Esc / B closes an open list first
    return;
  }
  if (select && !select->IsSelectBoxVisible() && (k == KI_LEFT || k == KI_RIGHT)) {
    // Left / Right step through a closed list's choices, like a slider.
    const int n = select->GetNumOptions(), i = select->GetSelection() + (k == KI_RIGHT ? 1 : -1);
    if (i >= 0 && i < n) select->SetSelection(i);
    return;
  }
  if (k == KI_ESCAPE) {
    patch::Log("MenuMaX: back (%s)", from);
    Close(from);
    return;
  }
  if (k == KI_F5) {
    g.signature = 0;
    g.last_watch = 0;
    patch::Log("MenuMaX: F5: reload");
    return;
  }
  if (!g.context) return;
  if (!IsControl(g.context->GetFocusElement()) && g.last_focus) g.last_focus->Focus(true);
  const int mods = g.shift ? KM_SHIFT : 0;
  g.context->ProcessKeyDown(k, mods);
  g.context->ProcessKeyUp(k, mods);
  if (g.doc) KeepFocus("key");
  if (GetConfig().menu_log) {
    Rml::Element* f = g.context->GetFocusElement();
    patch::Log("MenuMaX: key %d (%s) -> focus %s#%s", static_cast<int>(k), from, f ? f->GetTagName().c_str() : "-",
               f ? f->GetId().c_str() : "");
  }
}

void Hold(Rml::Input::KeyIdentifier k, bool down) {
  if (down) {
    g.held = k;
    g.held_since = g.held_last = GetTickCount();
  } else if (g.held == k) {
    g.held = Rml::Input::KI_UNKNOWN;
  }
}

// Controllers: 0 up, 1 right, 2 down, 3 left, -1 none.
int PovDir(DWORD pov) {
  if (LOWORD(pov) == 0xFFFF) return -1;
  return static_cast<int>(((pov + 4500) % 36000) / 9000);
}
const Rml::Input::KeyIdentifier kDirKeys[4] = {Rml::Input::KI_UP, Rml::Input::KI_RIGHT, Rml::Input::KI_DOWN,
                                               Rml::Input::KI_LEFT};

void PadDir(int dir, const char* from) {
  if (dir == g.pad_dir_prev) return;
  if (g.pad_dir_prev >= 0) Hold(kDirKeys[g.pad_dir_prev], false);
  g.pad_dir_prev = dir;
  if (dir >= 0) {
    Press(kDirKeys[dir], from);
    Hold(kDirKeys[dir], true);
  }
}

void PollPads() {
  // Gamepads first. DirectInput joysticks (POV + buttons 1 / 2) only without
  // the ControllerHub and while no XInput pad is there: an Xbox pad shows up
  // as both, and the hub's own joystick can be the keyboard (KeyboardPad).
  bool xpad = false;
  // Every pad's state in XInput's layout: from the ControllerHub (SDL3: every
  // make of gamepad) when it runs, otherwise from XInput's four slots.
  std::vector<hub::PadState> pads;
  const bool from_hub = hub::ReadAll(&pads);
  if (!from_hub) {
    if (XInputGetStateFn get = LoadXInput()) {
      for (DWORD i = 0; i < 4; ++i) {
        XPad s{};
        if (get(i, &s) != ERROR_SUCCESS) continue;
        pads.push_back(hub::PadState{s.lx, s.ly, s.rx, s.ry, s.lt, s.rt, s.buttons});
      }
    }
  }
  if (from_hub != g.pads_from_hub || pads.size() != g.pad_count) {
    g.pads_from_hub = from_hub;
    g.pad_count = pads.size();
    patch::Log("MenuMaX: %u controller(s) through %s", static_cast<unsigned>(pads.size()),
               from_hub ? "the ControllerHub (SDL3)" : "XInput");
  }
  // Every connected pad and joystick counts (a keyboard can show up as a pad
  // too); the first one held in a direction steers.
  int dir = -1;
  const char* dir_from = "controller";
  const bool settling = GetTickCount() - g.opened_at < 200;  // held when the screen opened
  if (!pads.empty()) {
    xpad = true;
    for (size_t i = 0; i < pads.size() && i < 8; ++i) {
      const hub::PadState& s = pads[i];
      const WORD pressed = settling ? 0 : static_cast<WORD>(s.buttons & ~g.pad_prev[i]);
      g.pad_prev[i] = s.buttons;
      if (settling) continue;
      int d = -1;
      if (s.buttons & 0x0001) d = 0;
      else if (s.buttons & 0x0008) d = 1;
      else if (s.buttons & 0x0002) d = 2;
      else if (s.buttons & 0x0004) d = 3;
      else if (s.ly > 20000) d = 0;
      else if (s.lx > 20000) d = 1;
      else if (s.ly < -20000) d = 2;
      else if (s.lx < -20000) d = 3;
      if (dir < 0) dir = d;
      if (pressed & 0x1000) Press(Rml::Input::KI_RETURN, "controller A");
      if (pressed & 0x2000) Press(Rml::Input::KI_ESCAPE, "controller B");
      if (pressed && GetConfig().menu_log) patch::Log("MenuMaX: controller %u buttons %04x", static_cast<unsigned>(i), s.buttons);
    }
  }
  // Wheels and other joysticks: through SDL with the hub (hat or centred
  // stick, button 1 = select, button 2 = back), else through the game's own
  // DirectInput reads (only while no XInput pad is there: an Xbox pad shows
  // up as both, and the hub's joystick can be the keyboard).
  std::vector<hub::StickState> sticks;
  if (from_hub) {
    hub::ReadSticks(&sticks);
    if (sticks.size() != g.stick_count) {
      g.stick_count = sticks.size();
      patch::Log("MenuMaX: %u other joystick(s) through SDL", static_cast<unsigned>(sticks.size()));
    }
  } else if (!xpad && g.joy_seen) {
    sticks.push_back(hub::StickState{PovDir(g.joy_pov), g.joy_buttons});
  }
  for (size_t i = 0; i < sticks.size() && i < 8; ++i) {
    const DWORD pressed = settling ? 0 : sticks[i].buttons & ~g.stick_prev[i];
    g.stick_prev[i] = sticks[i].buttons;
    if (settling) continue;
    if (dir < 0 && sticks[i].dir >= 0) {
      dir = sticks[i].dir;
      dir_from = "joystick";
    }
    if (pressed & 1) Press(Rml::Input::KI_RETURN, "joystick button 1");
    if (pressed & 2) Press(Rml::Input::KI_ESCAPE, "joystick button 2");
  }
  if (!settling && (!pads.empty() || !sticks.empty())) PadDir(dir, dir_from);
}

void DoInput(const RECT& game_rect) {
  std::vector<State::Key> keys;
  std::vector<std::pair<int, bool>> buttons;
  int wheel;
  {
    std::lock_guard<std::recursive_mutex> l(g.mu);
    keys.swap(g.keys);
    buttons.swap(g.buttons);
    wheel = g.wheel;
    g.wheel = 0;
  }
  // A key or button already down when the screen opened (the one that
  // opened it) is not a press on the screen.
  if (GetTickCount() - g.opened_at < 200) {
    for (auto& k : keys) k.down = false;
    for (auto& b : buttons) b.second = false;
    g.joy_buttons = 0;
  }
  // The pointer starts where the game's cursor was, then moves by the
  // mouse movement the game no longer gets: one unit = one menu pixel (as
  // the game's cursor), in window pixels.
  const float dp = (game_rect.bottom - game_rect.top) / 480.0f;
  int mdx, mdy;
  {
    std::lock_guard<std::recursive_mutex> l(g.mu);
    mdx = g.dx;
    mdy = g.dy;
    g.dx = g.dy = 0;
  }
  if (!g.pointer_set) {
    POINT c{};
    if (ModePromptCursor(&c)) {
      g.px = game_rect.left + (c.x + 0.5f) * dp;
      g.py = game_rect.top + (c.y + 0.5f) * dp;
    } else {
      g.px = (game_rect.left + game_rect.right) * 0.5f;
      g.py = (game_rect.top + game_rect.bottom) * 0.5f;
    }
    g.pointer_set = true;
    patch::Log("MenuMaX: pointer starts at %.0f,%.0f (game cursor %s)", g.px, g.py, ModePromptCursor(&c) ? "seen" : "not seen");
    mdx = mdy = 1;  // send a first position
  }
  if (mdx || mdy) {
    g.px = std::min(std::max(g.px + mdx * dp, 0.0f), static_cast<float>(g.context->GetDimensions().x - 1));
    g.py = std::min(std::max(g.py + mdy * dp, 0.0f), static_cast<float>(g.context->GetDimensions().y - 1));
    g.mouse_x = static_cast<int>(g.px);
    g.mouse_y = static_cast<int>(g.py);
    g.context->ProcessMouseMove(g.mouse_x, g.mouse_y, 0);
  }
  for (const auto& b : buttons) {
    if (b.second) g.context->ProcessMouseButtonDown(b.first, 0);
    else g.context->ProcessMouseButtonUp(b.first, 0);
    if (b.second && b.first == 0) {
      Rml::Element* h = g.context->GetHoverElement();
      if (GetConfig().menu_log || g.clicks < 20)
        patch::Log("MenuMaX: click at %d,%d on %s#%s", g.mouse_x, g.mouse_y, h ? h->GetTagName().c_str() : "-",
                   h ? h->GetId().c_str() : "");
    }
    if (b.second && b.first == 1) Press(Rml::Input::KI_ESCAPE, "right mouse button");
    if (!g.doc) return;
    if (b.first == 0) KeepFocus("click");
  }
  if (wheel) g.context->ProcessMouseWheel(Rml::Vector2f(0.0f, -wheel / 120.0f), 0);
  for (const auto& k : keys) {
    if (k.id == Rml::Input::KI_LSHIFT || k.id == Rml::Input::KI_RSHIFT) {
      g.shift = k.down;
      continue;
    }
    if (k.down) Press(k.id, "keyboard");
    if (!g.context || !g.active) return;
    if (Repeats(k.id)) Hold(k.id, k.down);
  }
  PollPads();
  // Held direction: repeat after 400 ms, every 90 ms.
  const DWORD now = GetTickCount();
  if (g.held != Rml::Input::KI_UNKNOWN && now - g.held_since > 400 && now - g.held_last >= 90) {
    g.held_last = now;
    Press(g.held, "repeat");
  }
}

// The screen's pointer: UI\MaX\images\pointer.png, its top left corner
// at the pointer, 22dp tall.
// Where a frame is drawn: the Direct3D 11 swap chain or the Direct3D 9
// back buffer.
struct Target {
  ID3D11RenderTargetView* rtv = nullptr;
  IDirect3DSurface9* surf9 = nullptr;
  UINT w = 0, h = 0;
};

void BeginDraw(const Target& t) {
  if (g.r9) {
    g.r9->Begin(t.surf9, t.w, t.h);
  } else {
    dd::ContextTouched();
    g.r11->Begin(dd::B().ctx, t.rtv, t.w, t.h);
  }
}

void EndDraw() {
  g.render->End();
  if (g.r11) dd::ContextTouched();
}

void DrawPointer(const Target& t, float dp) {
  if (!g.pointer_set) return;
  if (!g.pointer_tried) {
    g.pointer_tried = true;
    g.pointer_tex = g.render->LoadTexture(g.pointer_dims, Narrow(UiDir()) + "images\\pointer.png");
    if (!g.pointer_tex) patch::Log("MenuMaX: no images\\pointer.png; the screen has no pointer");
  }
  if (!g.pointer_tex || g.pointer_dims.y <= 0) return;
  const float hgt = 22.0f * dp, wid = hgt * g.pointer_dims.x / g.pointer_dims.y;
  const Rml::ColourbPremultiplied white(255, 255, 255, 255);
  Rml::Vertex v[4] = {{{0, 0}, white, {0, 0}}, {{wid, 0}, white, {1, 0}}, {{wid, hgt}, white, {1, 1}}, {{0, hgt}, white, {0, 1}}};
  const int idx[6] = {0, 1, 2, 0, 2, 3};
  Rml::CompiledGeometryHandle geo = g.render->CompileGeometry(Rml::Span<const Rml::Vertex>(v, 4), Rml::Span<const int>(idx, 6));
  if (!geo) return;
  BeginDraw(t);
  g.render->RenderGeometry(geo, Rml::Vector2f(std::floor(g.px), std::floor(g.py)), g.pointer_tex);
  EndDraw();
  g.render->ReleaseGeometry(geo);
}

void Report(bool force) {
  const DWORD now = GetTickCount();
  if (!force && now - g.last_report < 5000) return;
  g.last_report = now;
  Renderer::Stats& s = g.render->GetStats();
  patch::Log("MenuMaX: %s: %u frames, %u draws, %u new geometry, %u textures (%u failed), %u keys, %u clicks, %u saves, "
             "%u reloads, %u mouse moves, pointer %d,%d, %u doubled key presses ignored",
             g.open_name.c_str(), g.frames, s.draws, s.geometry, s.textures, s.texture_failures, g.key_events, g.clicks,
             g.saves, g.reloads, g.moves, g.mouse_x, g.mouse_y, g.dup_keys);
  s = Renderer::Stats{};
  g.frames = g.key_events = g.clicks = g.saves = g.reloads = g.moves = g.dup_keys = 0;
}

}  // namespace

// ---------------------------------------------------------------- API

void Install() {
  const Config& c = GetConfig();
  if (!c.menu_max) {
    patch::Log("MenuMaX: off (MenuMaX=0)");
    return;
  }
  g.installed = true;
  const DWORD attr = GetFileAttributesW((UiDir() + L"settings.rml").c_str());
  patch::Log("MenuMaX: on; screens in %ls (settings.rml %s); RmlUi starts when a screen first opens", UiDir().c_str(),
             attr == INVALID_FILE_ATTRIBUTES ? "MISSING" : "found");
}

bool Active() { return g.active; }

bool Open(const char* name, const char* why) {
  if (!g.installed) return false;
  if (g.failed) {
    patch::Log("MenuMaX: %s not opened (%s): RmlUi could not start earlier", name, why);
    return false;
  }
  std::lock_guard<std::recursive_mutex> l(g.mu);
  g.want_open = name;
  g.want_close = false;
  g.active = true;
  g.keys.clear();
  g.buttons.clear();
  std::memset(g.key_down, 0, sizeof g.key_down);
  g.last_key = Rml::Input::KI_UNKNOWN;
  g.last_key_source = -1;
  patch::Log("MenuMaX: open %s (%s)", name, why);
  return true;
}

void Close(const char* why) {
  std::lock_guard<std::recursive_mutex> l(g.mu);
  if (!g.active) return;
  g.want_close = true;
  g.want_open.clear();
  g.active = false;
  g.held = Rml::Input::KI_UNKNOWN;
  g.pad_dir_prev = -1;
  patch::Log("MenuMaX: close %s (%s)", g.open_name.c_str(), why);
}

namespace {

void PresentFrame(const Target& t, const RECT& game_rect) {
  const UINT w = t.w, h = t.h;
  if (!g.installed || g.failed) return;
  std::string open;
  bool close;
  {
    std::lock_guard<std::recursive_mutex> l(g.mu);
    open.swap(g.want_open);
    close = g.want_close;
    g.want_close = false;
  }
  if (close && g.doc) {
    CloseDoc();
    MenusRefreshFromIni();
    Report(true);
    patch::Log("MenuMaX: screen closed after %lu ms", static_cast<unsigned long>(GetTickCount() - g.opened_at));
  }
  if (!open.empty()) {
    if (!InitRml()) {
      Close("RmlUi could not start");
      return;
    }
    g.opened_at = GetTickCount();
    g.last_report = g.opened_at;
    g.pointer_set = false;
    if (!LoadDoc(open, std::string())) {
      Close("screen could not be loaded");
      return;
    }
  }
  if (!g.doc || !g.context) return;
  // Sizes: the window in pixels, 1dp = one 640x480 menu pixel.
  const Rml::Vector2i dims(static_cast<int>(w), static_cast<int>(h));
  if (g.context->GetDimensions() != dims) {
    g.context->SetDimensions(dims);
    patch::Log("MenuMaX: window %ux%u, menu area %ld,%ld %ldx%ld", w, h, game_rect.left, game_rect.top,
               game_rect.right - game_rect.left, game_rect.bottom - game_rect.top);
  }
  const float dp = (game_rect.bottom - game_rect.top) / 480.0f;
  if (std::fabs(g.context->GetDensityIndependentPixelRatio() - dp) > 0.001f) {
    g.context->SetDensityIndependentPixelRatio(dp);
    patch::Log("MenuMaX: 1dp = %.3f window pixels", dp);
  }
  DoInput(game_rect);
  if (!g.doc) return;  // closed by a key
  Watch();
  if (!g.doc) return;
  if (!g.pending.empty() && GetTickCount() - g.last_change > 300) FlushPending("settled");
  g.context->Update();
  BeginDraw(t);
  g.context->Render();
  EndDraw();
  DrawPointer(t, dp);
  ++g.frames;
  Report(false);
}

}  // namespace

void Present11(ID3D11RenderTargetView* rtv, UINT w, UINT h, const RECT& game_rect) {
  if (!g.installed || g.failed || dd::IsD3D9()) return;
  Target t;
  t.rtv = rtv;
  t.w = w;
  t.h = h;
  PresentFrame(t, game_rect);
}

void Present9(IDirect3DSurface9* back, UINT w, UINT h, const RECT& game_rect) {
  if (!g.installed || g.failed || !dd::IsD3D9() || !back) return;
  Target t;
  t.surf9 = back;
  t.w = w;
  t.h = h;
  PresentFrame(t, game_rect);
}

void DeviceGone9() {
  if (g.r9) g.r9->DeviceGone();
}

void KeyEvent(unsigned dik, bool down) {
  const Rml::Input::KeyIdentifier k = FromDik(dik);
  if (k == Rml::Input::KI_UNKNOWN) return;
  std::lock_guard<std::recursive_mutex> l(g.mu);
  // The same change reported twice (two ways of reading the keyboard).
  if (dik < 256) {
    if (g.key_down[dik] == down) return;
    g.key_down[dik] = down;
  }
  if (g.keys.size() < 256) g.keys.push_back({k, down});
}

void MouseButton(int button, bool down) {
  std::lock_guard<std::recursive_mutex> l(g.mu);
  if (g.buttons.size() < 64) g.buttons.emplace_back(button, down);
}

void MouseMove(int dx, int dy) {
  std::lock_guard<std::recursive_mutex> l(g.mu);
  g.dx += dx;
  g.dy += dy;
  ++g.moves;
}

void MouseWheel(int delta) {
  std::lock_guard<std::recursive_mutex> l(g.mu);
  g.wheel += delta;
}

void JoyState(DWORD pov, DWORD buttons) {
  std::lock_guard<std::recursive_mutex> l(g.mu);
  g.joy_seen = true;
  g.joy_pov = pov;
  g.joy_buttons = buttons;
}

}  // namespace ui
