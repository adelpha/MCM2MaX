#include "texreplace.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../config.h"
#include "../fpu_guard.h"
#include "../xp_compat.h"
#include "texdecode.h"

namespace dd {

namespace {

std::wstring UserDir() {
  const std::wstring local = xp::LocalAppDataW();
  if (local.empty()) return L"";
  return local + L"\\Motocross Madness 2";
}

std::wstring GameDir() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring s(buf);
  size_t slash = s.find_last_of(L'\\');
  return slash == std::wstring::npos ? L"." : s.substr(0, slash);
}

bool ParseHash(const wchar_t* name, uint64_t* out) {
  uint64_t v = 0;
  for (int i = 0; i < 16; ++i) {
    wchar_t c = name[i];
    int d;
    if (c >= L'0' && c <= L'9') d = c - L'0';
    else if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
    else if (c >= L'A' && c <= L'F') d = c - L'A' + 10;
    else return false;
    v = (v << 4) | static_cast<uint64_t>(d);
  }
  wchar_t next = name[16];
  if (next != L'.' && next != L'_' && next != L'-' && next != L' ') return false;
  *out = v;
  return true;
}

std::map<std::string, std::wstring>& NameIndex() {
  static std::map<std::string, std::wstring> index;
  return index;
}

std::string Narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) {
    if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
    s += c < 128 ? static_cast<char>(c) : '?';
  }
  return s;
}

// hash -> picture file, from both replacement folders (the per-user one wins).
// Pictures not named by a hash go into NameIndex() by file name.
const std::map<uint64_t, std::wstring>& Index() {
  static std::map<uint64_t, std::wstring> index;
  static bool built = false;
  if (built) return index;
  built = true;
  const std::wstring dirs[] = {GameDir() + L"\\textures", UserDir() + L"\\textures"};
  for (const std::wstring& dir : dirs) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      const wchar_t* ext = std::wcsrchr(fd.cFileName, L'.');
      if (!ext || (_wcsicmp(ext, L".png") && _wcsicmp(ext, L".jpg") && _wcsicmp(ext, L".jpeg") &&
                   _wcsicmp(ext, L".bmp") && _wcsicmp(ext, L".tga")))
        continue;
      uint64_t hash;
      if (ParseHash(fd.cFileName, &hash)) {
        index[hash] = dir + L"\\" + fd.cFileName;
      } else {
        std::wstring stem(fd.cFileName, static_cast<size_t>(ext - fd.cFileName));
        NameIndex()[Narrow(stem)] = dir + L"\\" + fd.cFileName;
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  Log("texture replacement: %u pictures named by hash, %u by texture name",
      static_cast<unsigned>(index.size()), static_cast<unsigned>(NameIndex().size()));
  return index;
}

}  // namespace

uint64_t TextureHash(const uint8_t* bgra, UINT w, UINT h) {
  // FNV-1a, 64-bit.
  uint64_t x = 14695981039346656037ull;
  auto mix = [&](uint8_t b) {
    x ^= b;
    x *= 1099511628211ull;
  };
  for (int i = 0; i < 4; ++i) mix(static_cast<uint8_t>(w >> (i * 8)));
  for (int i = 0; i < 4; ++i) mix(static_cast<uint8_t>(h >> (i * 8)));
  const size_t n = static_cast<size_t>(w) * h * 4;
  for (size_t i = 0; i < n; ++i) mix(bgra[i]);
  return x;
}

bool ExactTextureHashNeeded() {
  const Config& cfg = GetConfig();
  return cfg.texture_dump || (cfg.texture_replace && !Index().empty());
}

uint64_t FastTextureHash(const uint8_t* bgra, UINT w, UINT h) {
  // 8 bytes a step, multiply and fold (not the same values as TextureHash).
  uint64_t x = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(w) << 32 | h);
  const size_t n = static_cast<size_t>(w) * h * 4;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    std::memcpy(&v, bgra + i, 8);
    x = (x ^ v) * 0xff51afd7ed558ccdull;
    x ^= x >> 29;
  }
  for (; i < n; ++i) x = (x ^ bgra[i]) * 1099511628211ull;
  return x ^ (x >> 31);
}

namespace {

// Replacement pictures, by file. With TextureDecodeThread=1 (the default) the
// first request for a picture queues it for a decoding thread and the game
// keeps drawing its own texture until the picture is ready; the GPU texture
// is then made on the game thread (Direct3D calls stay there) and shared by
// every surface that uses that picture. TextureDecodeThread=0 decodes at the
// request, on the game thread.
struct Picture {
  enum State { kQueued, kDecoded, kFailed, kOnGpu };
  State state = kQueued;
  std::vector<uint8_t> px;  // decoded pixels until they are on the GPU
  UINT w = 0, h = 0;
  std::string err;
  std::string label;
  DWORD asked = 0;  // GetTickCount at the first request
  double decode_ms = 0;
  bool preload = false;  // TexturePreload queued it, not a request
  GpuTexture tex;
};

CRITICAL_SECTION g_cs;
HANDLE g_wake = nullptr;
std::map<std::wstring, Picture> g_pics;  // by path; under g_cs (tex: game thread only)
std::vector<std::wstring> g_queue;        // paths waiting for the decoding thread
volatile LONG g_generation = 0;           // bumped each time the thread finishes one
bool g_thread_failed = false;
// TexturePreload: progress (under g_cs).
struct PreloadStats {
  bool started = false, done_logged = false, uploads_logged = false;
  unsigned queued = 0, decoded = 0, failed = 0, over_cap = 0, uploaded = 0;
  double decoded_mb = 0, decode_ms = 0, upload_ms = 0, upload_max_frame_ms = 0;
  unsigned upload_frames = 0;
  LARGE_INTEGER t0{};
} g_pre;

void InitLock() {
  static bool done = false;
  if (done) return;
  done = true;
  InitializeCriticalSection(&g_cs);
}

double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
  static LARGE_INTEGER f = {};
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

void DecodeInto(Picture& p, const std::wstring& path) {
  // Called without g_cs held; the caller owns `p` until its state changes.
  std::vector<uint8_t> px;
  UINT w = 0, h = 0;
  std::string err;
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  const bool ok = DecodePicture(path, px, w, h, &err);
  QueryPerformanceCounter(&t1);
  EnterCriticalSection(&g_cs);
  p.px.swap(px);
  p.w = w;
  p.h = h;
  p.err = err;
  p.decode_ms = Ms(t0, t1);
  p.state = ok ? Picture::kDecoded : Picture::kFailed;
  LeaveCriticalSection(&g_cs);
}

DWORD WINAPI DecodeThread(void*) {
  for (;;) {
    WaitForSingleObject(g_wake, INFINITE);
    for (;;) {
      EnterCriticalSection(&g_cs);
      if (g_queue.empty()) {
        LeaveCriticalSection(&g_cs);
        break;
      }
      const std::wstring path = g_queue.front();
      g_queue.erase(g_queue.begin());
      Picture& p = g_pics[path];  // std::map: stays put
      const size_t left = g_queue.size();
      // TexturePreloadMB reached: the rest are left for when the game asks.
      if (p.preload && p.state == Picture::kQueued &&
          g_pre.decoded_mb >= static_cast<double>(GetConfig().texture_preload_mb)) {
        ++g_pre.over_cap;
        g_pics.erase(path);
        LeaveCriticalSection(&g_cs);
        InterlockedIncrement(&g_generation);  // a surface waiting on it asks again
        continue;
      }
      LeaveCriticalSection(&g_cs);
      DecodeInto(p, path);
      if (p.preload) {
        EnterCriticalSection(&g_cs);
        if (p.state == Picture::kFailed) {
          ++g_pre.failed;
        } else {
          ++g_pre.decoded;
          g_pre.decoded_mb += p.w * double(p.h) * 4 / 1048576.0;
        }
        g_pre.decode_ms += p.decode_ms;
        LeaveCriticalSection(&g_cs);
      }
      InterlockedIncrement(&g_generation);
      if (p.preload) continue;  // logged in total (TexturePreload)
      static unsigned logged = 0;
      if (++logged <= 300)
        Log("texture replacement \"%s\": decoded on the decoding thread, %ux%u in %.1f ms%s%s (%u more queued)",
            p.label.c_str(), p.w, p.h, p.decode_ms, p.state == Picture::kFailed ? ", failed: " : "",
            p.state == Picture::kFailed ? p.err.c_str() : "", static_cast<unsigned>(left));
    }
  }
}

bool StartDecodeThread() {
  static bool started = false;
  if (started) return true;
  if (g_thread_failed) return false;
  g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HANDLE t = g_wake ? CreateThread(nullptr, 0, DecodeThread, nullptr, 0, nullptr) : nullptr;
  if (!t) {
    g_thread_failed = true;
    Log("texture replacement: could not start the decoding thread; decoding on the game thread");
    return false;
  }
  SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
  CloseHandle(t);
  started = true;
  Log("texture replacement: decoding thread started (TextureDecodeThread=1)");
  return true;
}

// Decoded pixels -> the picture's GPU texture (game thread).
bool MakeTexture(Picture& p, std::vector<uint8_t>& px, double* ms) {
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  GpuTexture t;
  bool made;
  {
    FpuGuard fpu;
    made = CreateTextureFromBGRA(t, px.data(), p.w, p.h);
  }
  QueryPerformanceCounter(&t1);
  *ms = Ms(t0, t1);
  EnterCriticalSection(&g_cs);
  p.state = made ? Picture::kOnGpu : Picture::kFailed;
  if (!made) p.err = "could not create the texture";
  LeaveCriticalSection(&g_cs);
  if (!made) {
    LogOnce("texture replacement \"%s\": could not create a %ux%u texture", p.label.c_str(), p.w, p.h);
    return false;
  }
  p.tex = t;
  return true;
}

// kReplaced (out shares the picture's texture), kPending or kNone.
int GetPicture(const std::wstring& path, const char* label, GpuTexture& out) {
  InitLock();
  EnterCriticalSection(&g_cs);
  auto it = g_pics.find(path);
  const bool fresh = it == g_pics.end();
  Picture& p = fresh ? g_pics[path] : it->second;
  if (fresh) {
    p.label = label;
    p.asked = GetTickCount();
    if (GetConfig().texture_decode_thread && !g_thread_failed) {
      g_queue.push_back(path);
      LeaveCriticalSection(&g_cs);
      if (StartDecodeThread()) {
        SetEvent(g_wake);
        return kReplacePending;
      }
      EnterCriticalSection(&g_cs);  // no thread: decode here after all
      for (size_t i = 0; i < g_queue.size(); ++i)
        if (g_queue[i] == path) g_queue.erase(g_queue.begin() + static_cast<std::ptrdiff_t>(i)), i = g_queue.size();
    }
    LeaveCriticalSection(&g_cs);
    DecodeInto(p, path);
    EnterCriticalSection(&g_cs);
  }
  const Picture::State st = p.state;
  std::vector<uint8_t> px;
  if (st == Picture::kDecoded) px.swap(p.px);
  LeaveCriticalSection(&g_cs);
  if (st == Picture::kQueued) return kReplacePending;
  if (st == Picture::kFailed) {
    LogOnce("texture replacement \"%s\": could not read the picture (%s)", p.label.c_str(), p.err.c_str());
    return kReplaceNone;
  }
  if (st == Picture::kDecoded) {
    double ms = 0;
    if (!MakeTexture(p, px, &ms)) return kReplaceNone;
    static unsigned logged = 0;
    if (++logged <= 300)
      Log("texture replacement \"%s\": %ux%u (decoded in %.1f ms, texture made in %.1f ms on the game thread, in "
          "use %lu ms after the game first asked for it%s)",
          p.label.c_str(), p.w, p.h, p.decode_ms, ms, static_cast<unsigned long>(GetTickCount() - p.asked),
          p.preload ? "; preloaded, asked for before its texture was made" : "");
  }
  out.Reset();
  out = p.tex.Share();
  out.levels = p.tex.levels;
  return kReplaced;
}

std::wstring DumpDir() {
  static std::wstring dir;
  if (dir.empty()) {
    std::wstring user = UserDir();
    if (user.empty()) return L"";
    CreateDirectoryW(user.c_str(), nullptr);
    dir = user + L"\\textures_dump";
    CreateDirectoryW(dir.c_str(), nullptr);
  }
  return dir;
}

bool WritePng(const std::wstring& path, const uint8_t* bgra, UINT w, UINT h) {
  return WritePngFile(path, bgra, w, h);
}

std::string g_pending_name;
std::string g_copy_name;

}  // namespace

void SetPendingTextureName(const char* name) { g_pending_name = name ? name : ""; }

void SetCopyTextureName(const char* name) { g_copy_name = name ? name : ""; }

std::string TakeCopyTextureName() {
  std::string s;
  s.swap(g_copy_name);
  return s;
}

std::string TakePendingTextureName() {
  std::string s;
  s.swap(g_pending_name);
  return s;
}

std::string TextureKey(const char* name) {
  std::string s = name ? name : "";
  const size_t dot = s.find_last_of('.');
  const size_t sep = s.find_last_of("\\/");
  if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) s.resize(dot);
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    else if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
             c == '>' || c == '|' || c == ' ')
      c = '_';
  }
  while (!s.empty() && (s[0] == '_' || s[0] == '.')) s.erase(0, 1);
  return s;
}

int FindReplacement(const std::string& name, uint64_t hash, bool exact, GpuTexture& out) {
  if (!GetConfig().texture_replace) return kReplaceNone;
  const auto& index = Index();
  if (!name.empty()) {
    auto it = NameIndex().find(name);
    if (it != NameIndex().end()) {
      const int r = GetPicture(it->second, name.c_str(), out);
      if (r != kReplaceNone) return r;
    }
  }
  if (exact) {
    auto it = index.find(hash);
    if (it != index.end()) {
      char label[24];
      std::snprintf(label, sizeof(label), "%016llx", static_cast<unsigned long long>(hash));
      return GetPicture(it->second, label, out);
    }
  }
  return kReplaceNone;
}

unsigned ReplacementGeneration() { return static_cast<unsigned>(g_generation); }

// TexturePreload: the first call (first frame shown) queues every picture in
// the textures folders for the decoding thread; each call then makes GPU
// textures of decoded ones, for up to `budget_ms` (game thread), so the game
// finds them ready when it first loads the texture.
void ReplacementFrame(double budget_ms) {
  const Config& c = GetConfig();
  if (!c.texture_replace || !c.texture_preload || !c.texture_decode_thread) return;
  InitLock();
  if (!g_pre.started) {
    g_pre.started = true;
    QueryPerformanceCounter(&g_pre.t0);
    std::vector<std::pair<std::wstring, std::string>> todo;
    for (const auto& kv : Index()) {
      char label[24];
      std::snprintf(label, sizeof(label), "%016llx", static_cast<unsigned long long>(kv.first));
      todo.emplace_back(kv.second, label);
    }
    for (const auto& kv : NameIndex()) todo.emplace_back(kv.second, kv.first);
    EnterCriticalSection(&g_cs);
    for (const auto& t : todo) {
      if (g_pics.count(t.first)) continue;  // already asked for
      Picture& p = g_pics[t.first];
      p.label = t.second;
      p.asked = GetTickCount();
      p.preload = true;
      g_queue.push_back(t.first);
      ++g_pre.queued;
    }
    LeaveCriticalSection(&g_cs);
    Log("texture preload (TexturePreload=1): %u pictures queued for the decoding thread (up to %d MB decoded)",
        g_pre.queued, c.texture_preload_mb);
    if (g_pre.queued && StartDecodeThread()) SetEvent(g_wake);
    if (g_thread_failed) {
      EnterCriticalSection(&g_cs);  // no thread: forget them, the game's requests decode as before
      for (auto it = g_pics.begin(); it != g_pics.end();)
        it = it->second.preload && it->second.state == Picture::kQueued ? g_pics.erase(it) : std::next(it);
      g_queue.clear();
      LeaveCriticalSection(&g_cs);
      return;
    }
  }
  if (!g_pre.queued) return;
  LARGE_INTEGER f0, now;
  QueryPerformanceCounter(&f0);
  double frame_ms = 0;
  bool any = false;
  for (;;) {
    Picture* pick = nullptr;
    std::vector<uint8_t> px;
    EnterCriticalSection(&g_cs);
    for (auto& kv : g_pics)
      if (kv.second.preload && kv.second.state == Picture::kDecoded) {
        pick = &kv.second;
        px.swap(pick->px);
        break;
      }
    LeaveCriticalSection(&g_cs);
    if (!pick) break;
    double ms = 0;
    if (MakeTexture(*pick, px, &ms)) ++g_pre.uploaded;
    g_pre.upload_ms += ms;
    any = true;
    QueryPerformanceCounter(&now);
    frame_ms = Ms(f0, now);
    if (frame_ms >= budget_ms) break;
  }
  if (any) {
    ++g_pre.upload_frames;
    g_pre.upload_max_frame_ms = std::max(g_pre.upload_max_frame_ms, frame_ms);
    InterlockedIncrement(&g_generation);  // surfaces waiting on one look again
  }
  EnterCriticalSection(&g_cs);
  const bool all_decoded = g_pre.decoded + g_pre.failed + g_pre.over_cap >= g_pre.queued;
  LeaveCriticalSection(&g_cs);
  if (all_decoded && !g_pre.done_logged) {
    g_pre.done_logged = true;
    QueryPerformanceCounter(&now);
    Log("texture preload: decoding done in %.0f ms: %u decoded (%.1f MB, %.0f ms of decoding), %u failed, %u left for "
        "later (TexturePreloadMB %d reached)",
        Ms(g_pre.t0, now), g_pre.decoded, g_pre.decoded_mb, g_pre.decode_ms, g_pre.failed, g_pre.over_cap,
        c.texture_preload_mb);
  }
  if (all_decoded && !g_pre.uploads_logged && !any) {
    g_pre.uploads_logged = true;
    QueryPerformanceCounter(&now);
    Log("texture preload: all ready %.0f ms after the first frame: %u textures made in %.1f ms over %u frames (at most "
        "%.2f ms in one frame)",
        Ms(g_pre.t0, now), g_pre.uploaded, g_pre.upload_ms, g_pre.upload_frames, g_pre.upload_max_frame_ms);
  }
}

void ReplacementMemoryLog() {
  InitLock();
  // Also called from the crash report: don't wait for the decoding thread.
  if (!TryEnterCriticalSection(&g_cs)) {
    Log("memory: texture replacements: busy");
    return;
  }
  uint64_t bytes = 0;
  unsigned on_gpu = 0, queued = 0, failed = 0;
  for (const auto& kv : g_pics) {
    if (kv.second.state == Picture::kOnGpu) {
      ++on_gpu;
      bytes += static_cast<uint64_t>(kv.second.w) * kv.second.h * 4;
    } else if (kv.second.state == Picture::kFailed) {
      ++failed;
    } else {
      ++queued;
    }
  }
  LeaveCriticalSection(&g_cs);
  Log("memory: texture replacements: %u on the GPU (%u MB), %u waiting, %u failed", on_gpu,
      static_cast<unsigned>(bytes >> 20), queued, failed);
}

void DumpNamedTexture(const std::string& key, const uint8_t* bgra, UINT w, UINT h) {
  if (!GetConfig().texture_dump || key.empty()) return;
  // Keep the largest version the game streamed in.
  static std::map<std::string, uint64_t> biggest;
  const uint64_t area = static_cast<uint64_t>(w) * h;
  uint64_t& best = biggest[key];
  if (area <= best) return;
  best = area;
  const std::wstring dir = DumpDir();
  if (dir.empty()) return;
  std::wstring path = dir + L"\\";
  for (char c : key) path += static_cast<wchar_t>(static_cast<unsigned char>(c));
  path += L".png";
  if (!WritePng(path, bgra, w, h)) {
    LogOnce("texture dump failed for \"%s\"", key.c_str());
    return;
  }
  static unsigned logged = 0;
  if (++logged <= 300) Log("texture dump \"%s\" %ux%u", key.c_str(), w, h);
}

void DumpTexture(uint64_t hash, const uint8_t* bgra, UINT w, UINT h) {
  if (!GetConfig().texture_dump) return;
  static std::set<uint64_t> done;
  if (!done.insert(hash).second) return;
  const std::wstring dir = DumpDir();
  if (dir.empty()) return;
  wchar_t name[32];
  std::swprintf(name, 32, L"\\%016llx.png", static_cast<unsigned long long>(hash));
  std::wstring path = dir + name;
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
  if (!WritePng(path, bgra, w, h))
    LogOnce("texture dump failed for %016llx", static_cast<unsigned long long>(hash));
}

}  // namespace dd
