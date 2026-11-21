// MenuMaX: see menugame.h.
//
// Game facts (Mcm2.exe; see also menunav.cpp):
//   - Screens: the GUI manager's dialogs (tree iterator, class "UIDialog");
//     +0x148 modal; controls: iterator over [dialog + 0x7f3c], class
//     "UIControl". Control +0xf4 name, +0xc0 text (char*), +0x2c..+0x38 rect,
//     +0x6c enabled, +0x68 show/hide animation, +0x70 shown, +0x74 id, +0xb8 dialog, +0xbc its event
//     target, vtable = class.
//   - Events: dialog vtable +0x74 with {id, name, type, dialog, target,
//     control, 0, 0}; types 1 click, 2 list row, 3 change, 10 edit changed,
//     15 / 16 / 17 slider grab / release / held.
//   - Multi-state (checkbox, vtable 0x5582c8): +0x1f0 state, +0x1ec states.
//     Radio (0x5585e0): +0x1f0 state.
//   - Slider (0x5580b0): position 0x4764a0(this), set 0x476360(this, pos,
//     highest) (redraws), +0x200 highest position.
//   - List (0x5581b8): +0x1ec rows, +0x1f4 chosen (-1 none), +0x214 rows
//     (0x38 bytes: +4 bit 0 not selectable, +0x14 text). Choosing a row as a
//     click does (0x478e20): 0x4779f0(this, row) (tells controls linked to
//     the list, e.g. its dropdown), then vtable +0x108(&handled) (event 2).
//   - Dropdown (0x558b10): its list at +0x1fc.
//   - Edit box (0x557ea0): set text 0x474d40(this, text).
//   - Click (a KeyBind's calls, 0x472f00): 0x4886c0(ui, control, 0, 1), then
//     vtable +0xdc (press) and +0xe0 (release) with (0, 0); ui = [[control
//     + 0xb8] + 0x34].
#include "menugame.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "config.h"
#include "patch.h"

namespace mg {

namespace {

constexpr uintptr_t kGame = 0x5743AC;
constexpr uintptr_t kIterInit = 0x46A880, kIterNext = 0x46A980, kIterEnd = 0x46A970;
using IterInitFn = void(__fastcall*)(void* it, void* edx, uintptr_t root, int mode, const char* cls);
using IterNextFn = uintptr_t(__fastcall*)(void* it, void* edx);
using IterEndFn = void(__fastcall*)(void* it, void* edx);
using SetCaptureFn = int(__fastcall*)(uintptr_t ui, void* edx, uintptr_t control, uintptr_t* old, int now);
using PressFn = int(__fastcall*)(uintptr_t control, void* edx, int a, int b);
using GetFn = int(__fastcall*)(uintptr_t self, void* edx);
using SetPosFn = int(__fastcall*)(uintptr_t self, void* edx, int pos, int highest);
using IntFn = int(__fastcall*)(uintptr_t self, void* edx, int v);
using PtrFn = int(__fastcall*)(uintptr_t self, void* edx, void* p);
using TextFn = void(__fastcall*)(uintptr_t self, void* edx, const char* text);

struct KindOf {
  uintptr_t vtable;
  Kind kind;
};
const KindOf kKinds[] = {{0x557D98, kButton}, {0x5582C8, kCheck},  {0x5585E0, kRadio},
                         {0x558B10, kDropdown}, {0x557EA0, kEdit},  {0x5581B8, kList},
                         {0x5580B0, kSlider}, {0x557FA8, kScrollArrow}};

bool Readable(uintptr_t p, size_t n) { return p && !IsBadReadPtr(reinterpret_cast<const void*>(p), n); }
int I32(uintptr_t p) { return *reinterpret_cast<const int*>(p); }
uintptr_t Ptr(uintptr_t p) { return *reinterpret_cast<const uintptr_t*>(p); }

std::string Str(uintptr_t p, size_t max) {
  if (!p || IsBadStringPtrA(reinterpret_cast<const char*>(p), max)) return std::string();
  return std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), max));
}

bool Shown(uintptr_t node, uintptr_t top) {
  for (int depth = 0; node && depth < 16; ++depth) {
    if (!Readable(node, 0x28)) return false;
    const uint8_t f = *reinterpret_cast<const uint8_t*>(node + 0x25);
    if (!(f & 1) || (f & 8)) return false;
    if (node == top) return true;
    node = Ptr(node + 0x14);
  }
  return true;
}

bool Live(uintptr_t c) {
  if (!I32(c + 0x70)) return false;
  uintptr_t a = Ptr(c + 0x18C);
  for (int depth = 0; a && depth < 8; ++depth) {
    if (!Readable(a, 0x190)) return false;
    if (!I32(a + 0x70) || I32(a + 0x68) != 0) return false;
    a = Ptr(a + 0x18C);
  }
  return true;
}

uintptr_t Manager() {
  const uintptr_t game = Ptr(kGame);
  if (!Readable(game, 0x570)) return 0;
  const uintptr_t ui = Ptr(game + 0x56C);
  if (!Readable(ui, 0x30)) return 0;
  const uintptr_t mgr = Ptr(ui + 0x2C);
  return Readable(mgr, 0x310) ? mgr : 0;
}

// Every node under root in the GUI tree (+0x10 first child, +0xc next),
// whatever its class. Not the game's own iterator (0x46a880): that counts
// open walks in a global (0x661690) and while it is non-zero the game
// refuses to add controls to a dialog (0x46a070), which looked like what
// dropped Pro Circuit from a fresh main menu.
template <typename F>
void RawEach(uintptr_t root, F f) {
  if (!Readable(root, 0x18)) return;
  // Also follows +8 (previous): a control can drop out of its parent's
  // chain while its neighbour still points back at it (Pro Circuit on a
  // fresh main menu, until another dialog loads).
  std::vector<uintptr_t> stack;
  std::unordered_set<uintptr_t> seen;
  stack.push_back(Ptr(root + 0x10));
  for (int guard = 0; !stack.empty() && guard < 40000; ++guard) {
    const uintptr_t n = stack.back();
    stack.pop_back();
    if (!n || n == root || !Readable(n, 0x240)) continue;
    if (!seen.insert(n).second) continue;
    const uintptr_t vt = Ptr(n);
    if (vt >= 0x550000 && vt < 0x560000) f(n);
    stack.push_back(Ptr(n + 0x08));
    stack.push_back(Ptr(n + 0x0C));
    stack.push_back(Ptr(n + 0x10));
  }
}

bool IsDialog(uintptr_t n) {
  if (!Readable(n, 0x7F40)) return false;
  const std::string name = Str(n + 0x38, 64);
  return name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".dtm") == 0;
}

// The dialogs (cls "UIDialog") or controls (anything else) under root.
template <typename F>
void Each(uintptr_t root, const char* cls, F f) {
  const bool dialogs = std::strcmp(cls, "UIDialog") == 0;
  RawEach(root, [&](uintptr_t n) {
    if (IsDialog(n) == dialogs) f(n);
  });
}

struct Entry {
  std::string name;
  uintptr_t p;
  uintptr_t dialog;
  bool modal;
};

struct Action {
  enum Type { kClick, kSlider, kChoose, kText } type = kClick;
  std::string name;
  uintptr_t scope = 0;  // only on this dialog (0: any)
  int a = 0, b = 0;
  std::string text;
};

struct State {
  std::mutex mu;
  std::vector<Entry> controls;
  uintptr_t modal = 0;
  DWORD refreshed = 0;
  unsigned refreshes = 0;
  std::vector<Action> queue;
  DWORD tick_at = 0;
  unsigned done = 0;
  std::vector<uintptr_t> dialogs, logged;  // shown now / as last logged
} g;

const Entry* FindEntry(const char* name, uintptr_t scope = 0) {
  const Entry* best = nullptr;
  for (const Entry& e : g.controls) {
    if (e.name != name) continue;
    if (scope) {
      if (e.dialog == scope) return &e;
      continue;
    }
    if (e.modal) return &e;
    best = &e;  // the last shown dialog with it (the newest)
  }
  return best;
}

Kind KindFor(uintptr_t c) {
  const uintptr_t vt = Ptr(c);
  for (const KindOf& k : kKinds)
    if (k.vtable == vt) return k.kind;
  return kStatic;
}

// A list's rows (a dropdown's own list).
uintptr_t ListOf(uintptr_t c, Kind kind) {
  if (kind == kList) return c;
  if (kind != kDropdown) return 0;
  const uintptr_t l = Ptr(c + 0x1FC);
  return Readable(l, 0x220) ? l : 0;
}

void Send(uintptr_t c, int type) {
  const uintptr_t dlg = Ptr(c + 0xB8);
  if (!Readable(dlg, 8)) return;
  uintptr_t ev[8] = {static_cast<uintptr_t>(I32(c + 0x74)), c + 0xF4, static_cast<uintptr_t>(type), dlg, Ptr(c + 0xBC), c, 0, 0};
  reinterpret_cast<PtrFn>(Ptr(Ptr(dlg) + 0x74))(dlg, nullptr, ev);
}

void Do(const Action& a) {
  Refresh();
  const Entry* e = FindEntry(a.name.c_str(), a.scope);
  if (!e) {
    patch::Log("MenuMaX: game control %s is not on the screen now%s; nothing done", a.name.c_str(),
               a.scope ? " (on its dialog)" : "");
    return;
  }
  const uintptr_t c = e->p;
  const Kind kind = KindFor(c);
  if (!I32(c + 0x6C) || !Live(c)) {
    patch::Log("MenuMaX: game control %s can't be changed now (enabled %d, shown %d)", a.name.c_str(), I32(c + 0x6C), Live(c) ? 1 : 0);
    return;
  }
  ++g.done;
  switch (a.type) {
    case Action::kClick: {
      const uintptr_t owner = Ptr(c + 0xB8);
      const uintptr_t ui = Readable(owner, 0x38) ? Ptr(owner + 0x34) : 0;
      if (!Readable(ui, 0x1E0)) {
        patch::Log("MenuMaX: game control %s: no GUI to click it through", a.name.c_str());
        return;
      }
      patch::Log("MenuMaX: click game control %s", a.name.c_str());
      const uintptr_t vt = Ptr(c);
      reinterpret_cast<SetCaptureFn>(0x4886C0)(ui, nullptr, c, nullptr, 1);
      reinterpret_cast<PressFn>(Ptr(vt + 0xDC))(c, nullptr, 0, 0);
      reinterpret_cast<PressFn>(Ptr(vt + 0xE0))(c, nullptr, 0, 0);
      break;
    }
    case Action::kSlider: {
      if (kind != kSlider) break;
      const int highest = I32(c + 0x200);
      const int pos = a.a < 0 ? 0 : (highest > 0 && a.a > highest ? highest : a.a);
      const int before = reinterpret_cast<GetFn>(0x4764A0)(c, nullptr);
      if (pos != before) reinterpret_cast<SetPosFn>(0x476360)(c, nullptr, pos, highest);
      Send(c, a.b);
      if (GetConfig().menu_log || pos != before)
        patch::Log("MenuMaX: game slider %s %d -> %d of %d (event %d)", a.name.c_str(), before,
                   reinterpret_cast<GetFn>(0x4764A0)(c, nullptr), highest, a.b);
      break;
    }
    case Action::kChoose: {
      const uintptr_t l = ListOf(c, kind);
      if (!l) break;
      const int count = I32(l + 0x1EC);
      if (a.a < 0 || a.a >= count) {
        patch::Log("MenuMaX: game list %s has no row %d (%d rows)", a.name.c_str(), a.a, count);
        break;
      }
      const uintptr_t items = Ptr(l + 0x214);
      if (Readable(items, static_cast<size_t>(count) * 0x38) && (*reinterpret_cast<const uint8_t*>(items + a.a * 0x38 + 4) & 1)) {
        patch::Log("MenuMaX: game list %s row %d can't be chosen", a.name.c_str(), a.a);
        break;
      }
      const int before = I32(l + 0x1F4);
      reinterpret_cast<IntFn>(0x4779F0)(l, nullptr, a.a);
      int handled = 0;
      reinterpret_cast<PtrFn>(Ptr(Ptr(l) + 0x108))(l, nullptr, &handled);
      patch::Log("MenuMaX: game list %s row %d -> %d (\"%s\")", a.name.c_str(), before, I32(l + 0x1F4),
                 Readable(items, static_cast<size_t>(count) * 0x38) ? Str(Ptr(items + a.a * 0x38 + 0x14), 128).c_str() : "");
      break;
    }
    case Action::kText: {
      if (kind != kEdit) break;
      reinterpret_cast<TextFn>(0x474D40)(c, nullptr, a.text.c_str());
      Send(c, 10);
      patch::Log("MenuMaX: game edit box %s = \"%s\"", a.name.c_str(), Str(Ptr(c + 0xC0), 256).c_str());
      break;
    }
  }
}

void RunQueue(const char* where) {
  std::vector<Action> q;
  {
    std::lock_guard<std::mutex> l(g.mu);
    q.swap(g.queue);
  }
  if (q.empty()) return;
  if (GetConfig().menu_log) patch::Log("MenuMaX: %u game control change(s) %s", static_cast<unsigned>(q.size()), where);
  for (const Action& a : q) Do(a);
}

void Queue(Action a) {
  std::lock_guard<std::mutex> l(g.mu);
  // A newer position / text for the same control replaces one not yet made.
  if (a.type == Action::kSlider || a.type == Action::kText) {
    for (Action& o : g.queue)
      if (o.type == a.type && o.name == a.name && o.scope == a.scope && (a.type != Action::kSlider || o.b == a.b)) {
        o = a;
        return;
      }
  }
  if (g.queue.size() < 64) g.queue.push_back(std::move(a));
}

// The dialog's layout file, e.g. "optgame" (from "OptGame.dtm" at +0x38).
std::string DtmOf(uintptr_t d) {
  std::string n = Str(d + 0x38, 64);
  const size_t slash = n.find_last_of("\\/");
  if (slash != std::string::npos) n = n.substr(slash + 1);
  const size_t dot = n.find('.');
  if (dot != std::string::npos) n.resize(dot);
  for (char& c : n)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return n;
}

}  // namespace

void Refresh() {
  // The game's count of open GUI walks: non-zero between frames would make
  // it refuse new controls (0x46a070). Logged when it changes.
  static int walks = 0, walk_logs = 0;
  const int now_walks = *reinterpret_cast<const int*>(0x661690);
  if (now_walks != walks && walk_logs < 40) {
    ++walk_logs;
    patch::Log("MenuMaX: the game's open GUI walk count is %d (was %d)", now_walks, walks);
    walks = now_walks;
  }
  g.controls.clear();
  g.modal = 0;
  const uintptr_t mgr = Manager();
  if (!mgr) return;
  ++g.refreshes;
  std::vector<uintptr_t>& dialogs = g.dialogs;
  dialogs.clear();
  Each(mgr, "UIDialog", [&](uintptr_t d) {
    if (!Readable(d, 0x7F40) || !Shown(d, d)) return;
    dialogs.push_back(d);
    if (I32(d + 0x148)) g.modal = d;
  });
  for (uintptr_t d : dialogs) {
    const uintptr_t list = Ptr(d + 0x7F3C);
    if (!Readable(list, 0x28)) continue;
    Each(list, "UIControl", [&](uintptr_t c) {
      // Not only controls the GUI tree marks shown: some are left unmarked
      // while the game shows them (Pro Circuit on the main menu's Single
      // Player panel until another screen has been opened); +0x70 decides.
      if (!Readable(c, 0x240) || Ptr(c + 0xB8) != d) return;
      Entry e;
      e.name = Str(c + 0xF4, 64);
      if (e.name.empty()) return;
      e.p = c;
      e.dialog = d;
      e.modal = d == g.modal;
      g.controls.push_back(std::move(e));
    });
    // Controls under the dialog in the GUI tree but not (yet) in its list.
    const size_t listed = g.controls.size();
    RawEach(d, [&](uintptr_t c) {
      if (Ptr(c + 0xB8) != d) return;  // not one of this dialog's controls
      for (size_t k = 0; k < listed; ++k)
        if (g.controls[k].p == c) return;
      Entry e;
      e.name = Str(c + 0xF4, 64);
      if (e.name.empty()) return;
      e.p = c;
      e.dialog = d;
      e.modal = d == g.modal;
      g.controls.push_back(std::move(e));
    });
  }
  if (dialogs != g.logged) {
    g.logged = dialogs;
    patch::Log("MenuMaX: game dialogs shown now: %u", static_cast<unsigned>(dialogs.size()));
    for (uintptr_t d : dialogs) {
      std::string names;
      int n = 0;
      for (const Entry& e : g.controls)
        if (e.dialog == d && n++ < 6) names += " " + e.name;
      int total = 0;
      for (const Entry& e : g.controls) total += e.dialog == d;
      patch::Log("MenuMaX:   dialog %08X %s at %d,%d %dx%d%s, %d controls:%s%s", static_cast<unsigned>(d), DtmOf(d).c_str(),
                 I32(d + 0x160), I32(d + 0x164), I32(d + 0x168), I32(d + 0x16C), d == g.modal ? " (modal)" : "", total,
                 names.c_str(), total > 6 ? " ..." : "");
    }
  }
}

void Locate(const char* name) {
  const uintptr_t mgr = Manager();
  if (!mgr) return;
  int found = 0;
  RawEach(mgr, [&](uintptr_t c) {
    if (Str(c + 0xF4, 64) != name || found >= 4) return;
    ++found;
    std::string chain;
    uintptr_t n = c;
    for (int depth = 0; n && depth < 8 && Readable(n, 0x40); ++depth) {
      char b[96];
      std::snprintf(b, sizeof b, " %08X(flags %02X%s%s)", static_cast<unsigned>(n), *reinterpret_cast<const uint8_t*>(n + 0x25),
                    n == c ? "" : " ", n == c ? "" : DtmOf(n).c_str());
      chain += b;
      n = Ptr(n + 0x14);
    }
    patch::Log("MenuMaX: game control %s is in the GUI tree at%s; vtable %08X, class string \"%s\", dialog %08X, shown %d, animation %d",
               name, chain.c_str(), static_cast<unsigned>(Ptr(c)), Str(Ptr(c + 0x28), 64).c_str(),
               static_cast<unsigned>(Ptr(c + 0xB8)), I32(c + 0x70), I32(c + 0x68));
  });
  if (!found) patch::Log("MenuMaX: game control %s is nowhere in the GUI tree", name);
}

int DialogField(uintptr_t d, size_t off) { return Readable(d + off, 4) ? I32(d + off) : 0; }

void Dialogs(std::vector<Dialog>* out) {
  out->clear();
  for (uintptr_t d : g.dialogs) {
    Dialog x;
    x.p = d;
    x.modal = d == g.modal;
    x.dtm = DtmOf(d);
    x.rect = {I32(d + 0x160), I32(d + 0x164), I32(d + 0x168), I32(d + 0x16C)};
    for (const Entry& e : g.controls)
      if (e.dialog == d) x.names.push_back(e.name);
    out->push_back(std::move(x));
  }
}

bool Get(const char* name, Info* out, uintptr_t scope) {
  const Entry* e = FindEntry(name, scope);
  if (!e) return false;
  const uintptr_t c = e->p;
  Info i;
  i.kind = KindFor(c);
  // +0x68: show / hide animation running (1 in, 2 out; 0x471590). A pop-out
  // panel (FX=Anim) plays its frames before it counts as shown, so the copy
  // waits for the game's animation to finish, and goes as soon as it hides.
  i.live = Live(c);
  i.shown = i.live && I32(c + 0x68) == 0;
  i.tree = Shown(c, e->dialog);
  i.anim = I32(c + 0x68);
  if (i.anim == 1 || i.anim == 2) {
    // The show (+0x184) / hide (+0x188) animation: +8 frame, +0x10 frames.
    const uintptr_t a = Ptr(c + (i.anim == 1 ? 0x184 : 0x188));
    if (Readable(a, 0x14)) {
      i.frame = I32(a + 8);
      i.frames = I32(a + 0x10);
    }
  }
  i.enabled = I32(c + 0x6C) != 0;
  i.dialog = e->dialog;
  i.rect = {I32(c + 0x2C), I32(c + 0x30), I32(c + 0x34), I32(c + 0x38)};
  i.text = Str(Ptr(c + 0xC0), 256);
  switch (i.kind) {
    case kCheck:
      i.value = I32(c + 0x1F0);
      i.range = I32(c + 0x1EC);
      break;
    case kRadio:
      i.value = I32(c + 0x1F0);
      i.range = 2;
      break;
    case kSlider:
      i.value = reinterpret_cast<GetFn>(0x4764A0)(c, nullptr);
      i.range = I32(c + 0x200);
      break;
    case kList:
    case kDropdown:
      if (const uintptr_t l = ListOf(c, i.kind)) {
        i.value = I32(l + 0x1F4);
        i.range = I32(l + 0x1EC);
        i.top = I32(l + 0x1F0);
        i.visible = I32(l + 0x200);
      }
      break;
    default:
      break;
  }
  *out = i;
  return true;
}

bool Rows(const char* name, std::vector<std::string>* rows, uintptr_t scope) {
  rows->clear();
  const Entry* e = FindEntry(name, scope);
  if (!e) return false;
  const uintptr_t l = ListOf(e->p, KindFor(e->p));
  if (!l) return false;
  const int count = I32(l + 0x1EC);
  const uintptr_t items = Ptr(l + 0x214);
  if (count < 0 || count > 4096 || (count && !Readable(items, static_cast<size_t>(count) * 0x38))) return false;
  for (int r = 0; r < count; ++r) rows->push_back(Str(Ptr(items + r * 0x38 + 0x14), 256));
  return true;
}

bool Modal(std::vector<std::string>* names) {
  if (!g.modal) return false;
  if (names) {
    names->clear();
    for (const Entry& e : g.controls)
      if (e.modal) names->push_back(e.name);
  }
  return true;
}

namespace {
Action Make(Action::Type type, const char* name, uintptr_t scope, int a = 0, int b = 0) {
  Action x;
  x.type = type;
  x.name = name;
  x.scope = scope;
  x.a = a;
  x.b = b;
  return x;
}
}  // namespace

void Click(const char* name, uintptr_t scope) { Queue(Make(Action::kClick, name, scope)); }
void SetSlider(const char* name, int position, int event, uintptr_t scope) {
  Queue(Make(Action::kSlider, name, scope, position, event));
}
void Choose(const char* name, int row, uintptr_t scope) { Queue(Make(Action::kChoose, name, scope, row)); }
void SetText(const char* name, const std::string& text, uintptr_t scope) {
  Action a = Make(Action::kText, name, scope);
  a.text = text;
  Queue(std::move(a));
}

void Tick() {
  g.tick_at = GetTickCount();
  RunQueue("at the start of the game's tick");
}

void Frame() {
  if (GetTickCount() - g.tick_at > 500) RunQueue("from the menu cursor update (no tick seen)");
}

// ---- Diagnostics: who links / unlinks Pro Circuit and its neighbours ------
// The game's tree edits: 0x46a070 add child, 0x46a0d0 unlink, 0x46a140 insert
// after, 0x46a190 insert before. Each is logged (with the calling code) when
// it touches ButProCircuit, ButSupercross, ScreenOverCtl or the main menu's
// screen object (vtable 0x558c18).

namespace {
int g_tree_logs = 0;
uint8_t* g_tr_add = nullptr;
uint8_t* g_tr_unlink = nullptr;
uint8_t* g_tr_after = nullptr;
uint8_t* g_tr_before = nullptr;

std::string NodeName(uintptr_t n) {
  if (!n) return "null";
  if (!Readable(n, 0x200)) return "unreadable";
  if (Ptr(n) == 0x558C18) return "screen-object";
  const std::string s = Str(n + 0xF4, 64);
  return s.empty() ? "unnamed" : s;
}

bool Watched(uintptr_t n) {
  if (!n || !Readable(n, 0x200)) return false;
  if (Ptr(n) == 0x558C18) return true;
  const std::string s = Str(n + 0xF4, 64);
  return s == "ButProCircuit" || s == "ButSupercross" || s == "ScreenOverCtl";
}

std::string Links(uintptr_t n) {
  if (!n || !Readable(n, 0x200)) return "?";
  char buf[200];
  std::snprintf(buf, sizeof buf, "prev %s, next %s, parent %s, child %s", NodeName(Ptr(n + 8)).c_str(),
                NodeName(Ptr(n + 0xC)).c_str(), NodeName(Ptr(n + 0x14)).c_str(), NodeName(Ptr(n + 0x10)).c_str());
  return buf;
}

std::string Stack(const void* from) {
  char chain[120] = {};
  size_t used = 0;
  const uintptr_t* sp = static_cast<const uintptr_t*>(from);
  for (int k = 0, found = 0; k < 48 && found < 5; ++k) {
    const uintptr_t v = sp[k];
    if (v > 0x401000 && v < 0x550000) {
      used += std::snprintf(chain + used, sizeof chain - used, " %06X", static_cast<unsigned>(v));
      ++found;
    }
  }
  return chain;
}

void TreeLog(const char* what, uintptr_t node, uintptr_t other, uintptr_t ret, int result, const void* sp) {
  if (g_tree_logs >= 300) return;
  ++g_tree_logs;
  patch::Log("MenuMaX: tree: %s %s (%08X)%s%s from %06X -> %d; now %s; walks %d; stack%s", what, NodeName(node).c_str(),
             static_cast<unsigned>(node), other ? " / " : "", other ? NodeName(other).c_str() : "",
             static_cast<unsigned>(ret), result, Links(node).c_str(), *reinterpret_cast<const int*>(0x661690),
             Stack(sp).c_str());
}

int __fastcall TreeAddHook(uintptr_t list, void* edx, uintptr_t ctrl, int a) {
  const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  using Fn = int(__fastcall*)(uintptr_t, void*, uintptr_t, int);
  const int r = reinterpret_cast<Fn>(g_tr_add)(list, edx, ctrl, a);
  if (Watched(ctrl)) TreeLog("add child of", ctrl, list, ret, r, &ctrl);
  return r;
}

int __fastcall TreeUnlinkHook(uintptr_t ctrl, void* edx) {
  const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  const bool w = Watched(ctrl);
  const std::string before = w ? Links(ctrl) : std::string();
  using Fn = int(__fastcall*)(uintptr_t, void*);
  const int r = reinterpret_cast<Fn>(g_tr_unlink)(ctrl, edx);
  if (w) {
    if (g_tree_logs < 300) patch::Log("MenuMaX: tree: before unlink %s: %s", NodeName(ctrl).c_str(), before.c_str());
    TreeLog("unlink", ctrl, 0, ret, r, &ctrl);
  }
  return r;
}

int __fastcall TreeAfterHook(uintptr_t ctrl, void* edx, uintptr_t target, int a) {
  const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  using Fn = int(__fastcall*)(uintptr_t, void*, uintptr_t, int);
  const int r = reinterpret_cast<Fn>(g_tr_after)(ctrl, edx, target, a);
  if (Watched(ctrl) || Watched(target)) TreeLog("insert after", ctrl, target, ret, r, &ctrl);
  return r;
}

int __fastcall TreeBeforeHook(uintptr_t ctrl, void* edx, uintptr_t target, int a) {
  const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  using Fn = int(__fastcall*)(uintptr_t, void*, uintptr_t, int);
  const int r = reinterpret_cast<Fn>(g_tr_before)(ctrl, edx, target, a);
  if (Watched(ctrl) || Watched(target)) {
    TreeLog("insert before", ctrl, target, ret, r, &ctrl);
    if (Readable(target, 0x200) && g_tree_logs < 300)
      patch::Log("MenuMaX: tree:   target %s now %s", NodeName(target).c_str(), Links(target).c_str());
  }
  return r;
}

// Copies the first `n` bytes of the game's function (position independent:
// they only read absolute addresses) and jumps back after them.
uint8_t* Hook(uintptr_t at, const uint8_t* orig, size_t n, void* hook, const char* what) {
  if (!patch::Matches(at, orig, n)) {
    patch::Log("MenuMaX: tree diagnostics: %s not hooked (unexpected code at %06X)", what, static_cast<unsigned>(at));
    return nullptr;
  }
  uint8_t* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!t) return nullptr;
  std::memcpy(t, orig, n);
  t[n] = 0xE9;
  const int32_t rel = static_cast<int32_t>(at + n - (reinterpret_cast<uintptr_t>(t) + n + 5));
  std::memcpy(t + n + 1, &rel, 4);
  FlushInstructionCache(GetCurrentProcess(), t, 32);
  patch::Jump(at, reinterpret_cast<uintptr_t>(hook), n);
  return t;
}
}  // namespace

void InstallDiagnostics() {
  // mov eax,[0x661690]; test eax,eax / mov eax,[0x661690]; push esi
  static const uint8_t kAdd[] = {0xA1, 0x90, 0x16, 0x66, 0x00, 0x85, 0xC0};
  static const uint8_t kUnlink[] = {0xA1, 0x90, 0x16, 0x66, 0x00, 0x56};
  g_tr_add = Hook(0x46A070, kAdd, sizeof kAdd, reinterpret_cast<void*>(&TreeAddHook), "add child");
  g_tr_unlink = Hook(0x46A0D0, kUnlink, sizeof kUnlink, reinterpret_cast<void*>(&TreeUnlinkHook), "unlink");
  g_tr_after = Hook(0x46A140, kUnlink, sizeof kUnlink, reinterpret_cast<void*>(&TreeAfterHook), "insert after");
  g_tr_before = Hook(0x46A190, kUnlink, sizeof kUnlink, reinterpret_cast<void*>(&TreeBeforeHook), "insert before");
  patch::Log("MenuMaX: tree diagnostics: logging links and unlinks of Pro Circuit, Supercross, ScreenOverCtl and the screen object (%d of 4 hooked)",
             (g_tr_add != nullptr) + (g_tr_unlink != nullptr) + (g_tr_after != nullptr) + (g_tr_before != nullptr));
}

}  // namespace mg

