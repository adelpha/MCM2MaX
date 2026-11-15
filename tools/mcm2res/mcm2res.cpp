// mcm2res: list, view and extract what is inside Motocross Madness 2's
// resource archives (RES\*.RES, RES\*.TEX, UI\*.DAT, TERAFORM\*\*.ENV).
//
//   mcm2res [extract] [game folder] [-o folder] [--obj-step N] [--raw-all]
//   mcm2res list <archive>...
//   mcm2res check <folder of PNGs> [game folder]
//
// Double-clicked it finds the game next to or above itself and extracts
// everything into mcm2res_out\ next to the tool, with index.html to browse it.
// See tools/mcm2res/README.md.
//
// File formats (read from Mcm2.exe and checked against the data):
//   * Archives: optional "FAOE" header + byte cipher keyed by the lower-case
//     file name (k = 0xfa; k = ((k + 3) ^ c) + k per character; plain = k ^ x,
//     then k += x; every member decoded from its own start with the initial
//     key, 0x4628f0/0x461ce0). .TEX and ENGINE.RES are not encrypted. Then
//     "RS2\0", count, and per member: name length, name, offset, size, with
//     offsets counted from the end of the "FAOE" header (from the file start
//     when there is none).
//   * Texture records (the texture reader 0x4ca7d0, format table 0x516e20 =
//     bytes per pixel, 0x516f80 = surface format, 0x516f30 = has mipmaps,
//     0x516ee0 = may be compressed): a member "<name>.tga" in a RES is
//     u32 format, u32 x, u32 width, u32 height, u32 palette name length,
//     palette name, then either a mip table (formats with mipmaps) or the
//     pixels. A mip table is u32 offsets, each relative to its own position,
//     the first being the table size; the levels run from 1x1 up. A level (or
//     a whole picture without mipmaps) is raw when its size is
//     width*height*bpp, otherwise LZW (0x4a1940): a u32 header, then 9-bit
//     codes growing on code 0x101, 0x100 ends, 0x102 resets, MSB first.
//     Formats seen: 0x16 = 1:5:5:5 opaque with mips, 0x1a = 4:4:4:4 with
//     mips, 0x09 = 8:8:8, 0x0b = 4:4:4:4, 0x02 (sky boxes) = 5:5:5.
//   * Sky boxes (.cub in SKIES.RES) and the terrain's own textures (.trn)
//     hold bare "u32 format, mip table" records.
//   * Names: a texture loaded by name (every "<name>.tga" member: the game's
//     TextureMap loader 0x50fd00) is called <name> by mcm2fix (lower case, no
//     extension). One read from a stream (sky tiles, terrain textures) is
//     "<member>_<hex offset of its mip table in the decoded archive>"
//     (fixes_textures.cpp), e.g. terra12_51af51 or nat01_459aad.
//   * .trn terrain (track loader; see code/src/gputerrain.cpp): "TRN\0", 5, 1,
//     0, 256, 1, then blobs (u32 n; n bytes, raw if n is the expected size,
//     else an LZW stream whose header is n again): palette (768), two shading
//     tables (32768, 65536). Then u32 3, float scale (TerrainWidthScale),
//     u32 root level (2), u32 root record offset, u32 tile count twice, tile
//     offsets; u32 shared texture count twice, shared texture offsets. A tile
//     is three blobs (u32 n, u32 raw size, n bytes): 512 bytes, 580 bytes,
//     289 points x 8 bytes (17x17 row major, height = u16 at +2), then a
//     u32 base. A node record (0x47f550) is float s10, s14, ymin, ymax,
//     u16 ?, u16 tile, u32 texture code, 16 u32 sub-block codes, u32 n + n
//     bytes, a byte, u32 offset of its 256 children's offsets (0 = none).
//     Child i sits at x = (parent x + (i & 15)) * 16, z likewise with i >> 4,
//     one level (4 bits of shift) finer. Height = ((h - base) * s10 + base) *
//     s14 * scale. A texture code with bit 0 clear (and not 0xfffe/0xffff) is
//     shared texture (int16)code >> 1, spanning the node (256 wide) or one
//     cell (64 wide); texels map with uv * 0.9375 + 0.03125.
//   * .slt models (ECO.RES trees) are text: materials, then per LOD/surface
//     vertices (x, y, z, normal, u, v, ...) and faces.
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../mcm2sky/third_party/stb_image_write.h"

namespace {

// ---------------------------------------------------------------- files

#ifdef _WIN32
const char kSep = '\\';
std::wstring Wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}
std::string Utf8(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
  return s;
}
FILE* Open(const std::string& path, const char* mode) { return _wfopen(Wide(path).c_str(), Wide(mode).c_str()); }
bool IsDir(const std::string& p) {
  DWORD a = GetFileAttributesW(Wide(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
void MakeDir(const std::string& p) { CreateDirectoryW(Wide(p).c_str(), nullptr); }
std::vector<std::string> List(const std::string& dir) {
  std::vector<std::string> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(Wide(dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    std::string n = Utf8(fd.cFileName);
    if (n != "." && n != "..") out.push_back(n);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end());
  return out;
}
std::string ExeDir() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
  std::string p = Utf8(std::wstring(buf, n));
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? "." : p.substr(0, s);
}
std::string CurDir() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetCurrentDirectoryW(MAX_PATH * 2, buf);
  return Utf8(std::wstring(buf, n));
}
#else
const char kSep = '/';
FILE* Open(const std::string& path, const char* mode) { return fopen(path.c_str(), mode); }
bool IsDir(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
void MakeDir(const std::string& p) { mkdir(p.c_str(), 0755); }
std::vector<std::string> List(const std::string& dir) {
  std::vector<std::string> out;
  if (DIR* d = opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      std::string n = e->d_name;
      if (n != "." && n != "..") out.push_back(n);
    }
    closedir(d);
  }
  std::sort(out.begin(), out.end());
  return out;
}
std::string ExeDir() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return ".";
  std::string p(buf, n);
  return p.substr(0, p.find_last_of('/'));
}
std::string CurDir() {
  char buf[4096];
  return getcwd(buf, sizeof(buf)) ? buf : ".";
}
#endif

std::string Join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  char last = a.back();
  return (last == '\\' || last == '/') ? a + b : a + kSep + b;
}
std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}
std::string BaseName(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? p : p.substr(s + 1);
}
std::string DirName(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? "." : p.substr(0, s);
}
std::string StripExt(const std::string& n) {
  size_t d = n.find_last_of('.');
  size_t s = n.find_last_of("\\/");
  return (d == std::string::npos || (s != std::string::npos && d < s)) ? n : n.substr(0, d);
}
std::string Ext(const std::string& n) {
  std::string b = BaseName(n);
  size_t d = b.find_last_of('.');
  return d == std::string::npos ? "" : Lower(b.substr(d + 1));
}
std::string FindIn(const std::string& dir, const std::string& name) {
  std::string want = Lower(name);
  for (const std::string& e : List(dir))
    if (Lower(e) == want) return Join(dir, e);
  return "";
}
void MakeDirs(const std::string& p) {
  if (p.empty() || IsDir(p)) return;
  std::string parent = DirName(p);
  if (parent != p && parent != "." && !parent.empty()) MakeDirs(parent);
  MakeDir(p);
}
bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = Open(path, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) {
    fclose(f);
    return false;
  }
  out.resize(static_cast<size_t>(n));
  bool ok = n == 0 || fread(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}
bool WriteFile(const std::string& path, const void* data, size_t len) {
  FILE* f = Open(path, "wb");
  if (!f) return false;
  bool ok = len == 0 || fwrite(data, 1, len, f) == len;
  return fclose(f) == 0 && ok;
}
bool WriteText(const std::string& path, const std::string& s) { return WriteFile(path, s.data(), s.size()); }
// A name safe as a file name (members are plain names already, but be careful).
std::string SafeFile(std::string s) {
  for (char& c : s)
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
        c == '|' || static_cast<unsigned char>(c) < 32)
      c = '_';
  if (s.empty()) s = "_";
  return s;
}
// mcm2fix's TextureKey (dd/texreplace.cpp).
std::string TextureKey(const std::string& name) {
  std::string s = StripExt(name);
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    else if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
             c == '|' || c == ' ')
      c = '_';
  }
  while (!s.empty() && (s[0] == '_' || s[0] == '.')) s.erase(0, 1);
  return s;
}
std::string Hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%x", v);
  return b;
}
std::string Fmt(const char* f, ...) {
  char b[1024];
  va_list a;
  va_start(a, f);
  std::vsnprintf(b, sizeof(b), f, a);
  va_end(a);
  return b;
}
std::string HtmlEsc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}
// Path for a link in index.html (forward slashes, spaces escaped).
std::string Url(std::string s) {
  std::string o;
  for (char c : s) {
    if (c == '\\') o += '/';
    else if (c == ' ') o += "%20";
    else if (c == '#') o += "%23";
    else if (c == '%') o += "%25";
    else o += c;
  }
  return o;
}

uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
float F32(const uint8_t* p) {
  float f;
  std::memcpy(&f, p, 4);
  return f;
}

// ---------------------------------------------------------------- log

FILE* g_log = nullptr;
void Log(const char* f, ...) {
  char b[2048];
  va_list a;
  va_start(a, f);
  std::vsnprintf(b, sizeof(b), f, a);
  va_end(a);
  if (g_log) std::fprintf(g_log, "%s\n", b);
}
void Say(const char* f, ...) {
  char b[2048];
  va_list a;
  va_start(a, f);
  std::vsnprintf(b, sizeof(b), f, a);
  va_end(a);
  std::printf("%s\n", b);
  std::fflush(stdout);
  if (g_log) std::fprintf(g_log, "%s\n", b);
}

// ---------------------------------------------------------------- archives

struct Member {
  std::string name;
  uint32_t offset = 0, size = 0;
};

class Archive {
 public:
  bool Load(const std::string& path, std::string& err) {
    path_ = path;
    name_ = BaseName(path);
    if (!ReadFile(path, file_)) {
      err = "could not read the file";
      return false;
    }
    if (file_.size() >= 4 && std::memcmp(file_.data(), "FAOE", 4) == 0) {
      encrypted_ = true;
      base_ = 4;
      key_ = 0xfa;
      for (char c : Lower(name_)) key_ = static_cast<uint8_t>(((key_ + 3) ^ static_cast<uint8_t>(c)) + key_);
    } else if (file_.size() >= 4 && std::memcmp(file_.data(), "RS2", 4) == 0) {
      encrypted_ = false;
      base_ = 0;
    } else {
      err = "not an RS2 archive";
      return false;
    }
    const uint32_t avail = static_cast<uint32_t>(file_.size() - base_);
    std::vector<uint8_t> dir;
    Read(0, std::min<uint32_t>(avail, 1u << 20), dir);
    if (dir.size() < 8 || std::memcmp(dir.data(), "RS2", 4) != 0) {
      err = "directory not found (wrong file name for the key?)";
      return false;
    }
    uint32_t n = U32(&dir[4]);
    size_t p = 8;
    for (uint32_t i = 0; i < n; ++i) {
      if (p + 4 > dir.size()) break;
      uint32_t len = U32(&dir[p]);
      if (len > 4096 || p + 4 + len + 8 > dir.size()) break;
      Member m;
      m.name.assign(reinterpret_cast<const char*>(&dir[p + 4]), strnlen(reinterpret_cast<const char*>(&dir[p + 4]), len));
      m.offset = U32(&dir[p + 4 + len]);
      m.size = U32(&dir[p + 8 + len]);
      p += 12 + len;
      if (m.offset + static_cast<uint64_t>(m.size) > avail) {
        Log("  %s: member %s runs past the end of the file, skipped", name_.c_str(), m.name.c_str());
        continue;
      }
      members_.push_back(m);
    }
    if (members_.empty() && n) {
      err = "bad directory";
      return false;
    }
    return true;
  }
  void Read(uint32_t off, uint32_t len, std::vector<uint8_t>& out) const {
    const size_t start = base_ + static_cast<size_t>(off);
    if (start >= file_.size()) {
      out.clear();
      return;
    }
    len = static_cast<uint32_t>(std::min<size_t>(len, file_.size() - start));
    out.assign(file_.begin() + start, file_.begin() + start + len);
    if (!encrypted_) return;
    uint8_t k = key_;
    for (uint8_t& c : out) {
      uint8_t x = c;
      c = k ^ x;
      k = static_cast<uint8_t>(k + x);
    }
  }
  void ReadMember(const Member& m, std::vector<uint8_t>& out) const { Read(m.offset, m.size, out); }
  const std::vector<Member>& members() const { return members_; }
  const std::string& name() const { return name_; }
  const std::string& path() const { return path_; }
  bool encrypted() const { return encrypted_; }
  void Free() {
    std::vector<uint8_t>().swap(file_);
  }

 private:
  std::string path_, name_;
  std::vector<uint8_t> file_;
  bool encrypted_ = false;
  size_t base_ = 0;
  uint8_t key_ = 0;
  std::vector<Member> members_;
};

// ---------------------------------------------------------------- LZW (0x4a1940)

// `src` starts with the stream's 32-bit header.
bool Lzw(const uint8_t* src, size_t len, size_t want, std::vector<uint8_t>& out) {
  out.clear();
  out.reserve(want);
  size_t total_bits = len * 8, pos = 32;
  auto read = [&](int n) -> int {
    if (pos + n > total_bits) return 0x100;
    uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++pos) v = (v << 1) | ((src[pos >> 3] >> (7 - (pos & 7))) & 1);
    return static_cast<int>(v);
  };
  const int kMax = 1 << 16;
  static std::vector<int> prefix(kMax);
  static std::vector<uint8_t> chr(kMax);
  std::vector<uint8_t> stack;
  int next = 0x103, width = 9;
  int old = read(width);
  if (old == 0x100) return out.size() >= want;
  if (old > 0xff) return false;
  out.push_back(static_cast<uint8_t>(old));
  uint8_t fin = static_cast<uint8_t>(old);
  while (out.size() < want) {
    int c = read(width);
    if (c == 0x100) break;
    if (c == 0x102) {
      next = 0x103;
      width = 9;
      old = read(width);
      if (old == 0x100) break;
      if (old > 0xff) return false;
      out.push_back(static_cast<uint8_t>(old));
      fin = static_cast<uint8_t>(old);
      continue;
    }
    if (c == 0x101) {
      if (++width > 16) return false;
      continue;
    }
    stack.clear();
    int cx;
    if (c >= next) {
      if (c > next) return false;
      stack.push_back(fin);
      cx = old;
    } else {
      cx = c;
    }
    while (cx > 0xff) {
      if (cx >= next || stack.size() > static_cast<size_t>(kMax)) return false;
      stack.push_back(chr[cx]);
      cx = prefix[cx];
    }
    stack.push_back(static_cast<uint8_t>(cx));
    fin = static_cast<uint8_t>(cx);
    out.insert(out.end(), stack.rbegin(), stack.rend());
    if (next < kMax) {
      prefix[next] = old;
      chr[next] = fin;
      ++next;
    }
    old = c;
  }
  if (out.size() < want) return false;
  out.resize(want);
  return true;
}

// ---------------------------------------------------------------- pictures

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // w*h*4
};

std::vector<uint8_t> g_png;
void PngSink(void* ctx, void* data, int size) {
  (void)ctx;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  g_png.insert(g_png.end(), p, p + size);
}
bool SavePng(const std::string& path, int w, int h, int comp, const uint8_t* px) {
  g_png.clear();
  if (!stbi_write_png_to_func(PngSink, nullptr, w, h, comp, px, w * comp)) return false;
  return WriteFile(path, g_png.data(), g_png.size());
}
bool SavePng(const std::string& path, const Image& im) { return SavePng(path, im.w, im.h, 4, im.rgba.data()); }

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      table[i] = v;
    }
    init = true;
  }
  c = ~c;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return ~c;
}
// 16-bit greyscale PNG (stb_image_write only writes 8-bit).
bool SavePng16(const std::string& path, int w, int h, const std::vector<uint16_t>& v) {
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(h) * (1 + 2 * w));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      uint16_t s = v[static_cast<size_t>(y) * w + x];
      raw.push_back(static_cast<uint8_t>(s >> 8));
      raw.push_back(static_cast<uint8_t>(s & 0xff));
    }
  }
  int zlen = 0;
  unsigned char* z = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &zlen, 6);
  if (!z) return false;
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  auto chunk = [&](const char* type, const uint8_t* data, size_t n) {
    uint8_t len[4] = {static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 8),
                      static_cast<uint8_t>(n)};
    out.insert(out.end(), len, len + 4);
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (n) out.insert(out.end(), data, data + n);
    uint32_t c = Crc32(&out[start], 4 + n);
    uint8_t cb[4] = {static_cast<uint8_t>(c >> 24), static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8),
                     static_cast<uint8_t>(c)};
    out.insert(out.end(), cb, cb + 4);
  };
  uint8_t ihdr[13] = {static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 8),
                      static_cast<uint8_t>(w),       static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16),
                      static_cast<uint8_t>(h >> 8),  static_cast<uint8_t>(h),       16, 0, 0, 0, 0};
  chunk("IHDR", ihdr, 13);
  chunk("IDAT", z, static_cast<size_t>(zlen));
  chunk("IEND", nullptr, 0);
  STBIW_FREE(z);
  return WriteFile(path, out.data(), out.size());
}

uint8_t Expand5(unsigned v) { return static_cast<uint8_t>((v * 255 + 15) / 31); }  // as mcm2fix's dump
uint8_t Expand6(unsigned v) { return static_cast<uint8_t>((v * 255 + 31) / 63); }

// Plain TGA (types 1/2/3 and RLE 9/10/11; 8/15/16/24/32 bits).
bool DecodeTga(const uint8_t* d, size_t n, Image& im, std::string& err) {
  if (n < 18) {
    err = "too short";
    return false;
  }
  const int idlen = d[0], cmtype = d[1], type = d[2];
  const int cmfirst = U16(d + 3), cmlen = U16(d + 5), cmbits = d[7];
  const int w = U16(d + 12), h = U16(d + 14), bpp = d[16], desc = d[17];
  const int base = type & ~8;
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (base != 1 && base != 2 && base != 3)) {
    err = "unsupported TGA";
    return false;
  }
  size_t p = 18 + idlen;
  std::vector<uint8_t> cmap;  // RGBA
  if (cmtype == 1) {
    const int cb = (cmbits + 7) / 8;
    cmap.resize(static_cast<size_t>(cmlen) * 4);
    for (int i = 0; i < cmlen; ++i) {
      if (p + cb > n) break;
      const uint8_t* s = d + p;
      uint8_t* o = &cmap[static_cast<size_t>(i) * 4];
      if (cb == 2) {
        unsigned v = U16(s);
        o[0] = Expand5((v >> 10) & 31), o[1] = Expand5((v >> 5) & 31), o[2] = Expand5(v & 31), o[3] = 255;
      } else {
        o[0] = s[2], o[1] = s[1], o[2] = s[0], o[3] = cb == 4 ? s[3] : 255;
      }
      p += cb;
    }
  }
  const int pb = (bpp + 7) / 8;
  if (pb < 1 || pb > 4) {
    err = "unsupported bit depth";
    return false;
  }
  im.w = w;
  im.h = h;
  im.rgba.assign(static_cast<size_t>(w) * h * 4, 255);
  auto put = [&](size_t i, const uint8_t* s) {
    uint8_t* o = &im.rgba[i * 4];
    if (base == 1) {
      unsigned idx = pb == 2 ? U16(s) : s[0];
      idx -= cmfirst;
      if (idx < static_cast<unsigned>(cmlen) && !cmap.empty()) std::memcpy(o, &cmap[idx * 4], 4);
    } else if (base == 3) {
      o[0] = o[1] = o[2] = s[0];
    } else if (pb == 2) {
      unsigned v = U16(s);
      o[0] = Expand5((v >> 10) & 31), o[1] = Expand5((v >> 5) & 31), o[2] = Expand5(v & 31);
      o[3] = (desc & 15) ? ((v & 0x8000) ? 255 : 0) : 255;
    } else {
      o[0] = s[2], o[1] = s[1], o[2] = s[0], o[3] = pb == 4 ? s[3] : 255;
    }
  };
  const size_t total = static_cast<size_t>(w) * h;
  size_t i = 0;
  if (type & 8) {
    while (i < total && p < n) {
      int hdr = d[p++];
      int cnt = (hdr & 127) + 1;
      if (hdr & 128) {
        if (p + pb > n) break;
        for (int k = 0; k < cnt && i < total; ++k) put(i++, d + p);
        p += pb;
      } else {
        for (int k = 0; k < cnt && i < total && p + pb <= n; ++k, p += pb) put(i++, d + p);
      }
    }
  } else {
    for (; i < total && p + pb <= n; ++i, p += pb) put(i, d + p);
  }
  if (!(desc & 0x20)) {  // bottom-up: flip
    for (int y = 0; y < h / 2; ++y)
      std::swap_ranges(im.rgba.begin() + static_cast<size_t>(y) * w * 4,
                       im.rgba.begin() + static_cast<size_t>(y + 1) * w * 4,
                       im.rgba.begin() + static_cast<size_t>(h - 1 - y) * w * 4);
  }
  if (desc & 0x10) {  // right-to-left
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w / 2; ++x)
        for (int c = 0; c < 4; ++c)
          std::swap(im.rgba[(static_cast<size_t>(y) * w + x) * 4 + c],
                    im.rgba[(static_cast<size_t>(y) * w + (w - 1 - x)) * 4 + c]);
  }
  return true;
}

// ---------------------------------------------------------------- game texture formats

// The game's format table (0x516e20 bytes per pixel, 0x516f80 surface format,
// 0x516f30 mipmapped).
int FormatBpp(uint32_t f) {
  static const int t[] = {0, 3, 2, 2, 1, 2, 1, 2, 2, 3, 2, 2, 4, 2, 2, 4, 3, 2,
                          2, 1, 2, 1, 2, 2, 3, 2, 2, 4, 2, 2, 4, 2, 4, 2, 4};
  return f < sizeof(t) / sizeof(t[0]) ? t[f] : 0;
}
bool FormatMips(uint32_t f) { return (f >= 0x10 && f <= 0x1e) || f == 0x21 || f == 0x22; }
// Pixel layout: 555, 565, 888, PAL8, 1555, 4444, 8888.
enum Layout { kNoLayout, k555, k565, k888, kPal8, k1555, k4444, k8888 };
Layout FormatLayout(uint32_t f) {
  switch (f) {
    case 1: case 9: case 0x10: case 0x18: return k888;
    case 2: case 5: case 7: case 0x11: case 0x14: case 0x16: return k555;
    case 3: case 8: case 0x12: case 0x17: return k565;
    case 4: case 6: case 0x13: case 0x15: return kPal8;
    case 0xa: case 0xd: case 0x19: case 0x1c: case 0x1f: case 0x21: return k1555;
    case 0xb: case 0xe: case 0x1a: case 0x1d: return k4444;
    case 0xc: case 0xf: case 0x1b: case 0x1e: case 0x20: case 0x22: return k8888;
    default: return kNoLayout;
  }
}
const char* LayoutName(Layout l) {
  switch (l) {
    case k555: return "5:5:5";
    case k565: return "5:6:5";
    case k888: return "8:8:8";
    case kPal8: return "8-bit palette";
    case k1555: return "1:5:5:5";
    case k4444: return "4:4:4:4";
    case k8888: return "8:8:8:8";
    default: return "?";
  }
}
bool LayoutAlpha(Layout l) { return l == k1555 || l == k4444 || l == k8888; }

std::map<std::string, std::vector<uint8_t>> g_palettes;  // lower-case .pal8 name -> 768 bytes RGB

void Convert(Layout l, const uint8_t* src, int w, int h, const std::string& pal, Image& im) {
  im.w = w;
  im.h = h;
  im.rgba.assign(static_cast<size_t>(w) * h * 4, 255);
  const std::vector<uint8_t>* p = nullptr;
  if (l == kPal8) {
    auto it = g_palettes.find(Lower(pal));
    if (it != g_palettes.end()) p = &it->second;
  }
  for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
    uint8_t* o = &im.rgba[i * 4];
    switch (l) {
      case k555: {
        unsigned v = U16(src + 2 * i);
        o[0] = Expand5((v >> 10) & 31), o[1] = Expand5((v >> 5) & 31), o[2] = Expand5(v & 31);
        break;
      }
      case k565: {
        unsigned v = U16(src + 2 * i);
        o[0] = Expand5((v >> 11) & 31), o[1] = Expand6((v >> 5) & 63), o[2] = Expand5(v & 31);
        break;
      }
      case k1555: {
        unsigned v = U16(src + 2 * i);
        o[0] = Expand5((v >> 10) & 31), o[1] = Expand5((v >> 5) & 31), o[2] = Expand5(v & 31);
        o[3] = (v & 0x8000) ? 255 : 0;
        break;
      }
      case k4444: {
        unsigned v = U16(src + 2 * i);
        o[0] = static_cast<uint8_t>(((v >> 8) & 15) * 17), o[1] = static_cast<uint8_t>(((v >> 4) & 15) * 17);
        o[2] = static_cast<uint8_t>((v & 15) * 17), o[3] = static_cast<uint8_t>(((v >> 12) & 15) * 17);
        break;
      }
      case k888:  // stored R, G, B (MODELTGA.RES pictures)
        o[0] = src[3 * i], o[1] = src[3 * i + 1], o[2] = src[3 * i + 2];
        break;
      case k8888:
        o[0] = src[4 * i + 2], o[1] = src[4 * i + 1], o[2] = src[4 * i], o[3] = src[4 * i + 3];
        break;
      case kPal8: {
        uint8_t v = src[i];
        if (p) o[0] = (*p)[v * 3], o[1] = (*p)[v * 3 + 1], o[2] = (*p)[v * 3 + 2];
        else o[0] = o[1] = o[2] = v;
        break;
      }
      default:
        break;
    }
  }
}

// One picture: raw if `len` is exactly the size, else an LZW stream.
bool DecodeLevel(const uint8_t* p, size_t len, size_t want, std::vector<uint8_t>& out) {
  if (len == want) {
    out.assign(p, p + len);
    return true;
  }
  return Lzw(p, len, want, out);
}

// A mip table at d[p] (within `n` bytes). Returns the number of entries and
// absolute level offsets, or 0 if it doesn't look like one.
int MipTable(const uint8_t* d, size_t n, size_t p, int bpp, std::vector<size_t>& offs) {
  if (p + 12 > n) return 0;
  uint32_t t0 = U32(d + p);
  if (t0 % 4 || t0 < 12 || t0 > 64) return 0;
  int k = static_cast<int>(t0 / 4);
  if (p + t0 > n) return 0;
  offs.resize(k);
  for (int i = 0; i < k; ++i) offs[i] = p + 4 * static_cast<size_t>(i) + U32(d + p + 4 * i);
  for (int i = 1; i < k; ++i)
    if (offs[i] <= offs[i - 1] || offs[i] > n) return 0;
  if (offs[0] != p + t0) return 0;
  // The 1x1 and 2x2 levels are always raw.
  if (static_cast<int>(offs[1] - offs[0]) != bpp) return 0;
  if (k >= 4 && static_cast<int>(offs[2] - offs[1]) != 4 * bpp) return 0;
  return k;
}

// ---------------------------------------------------------------- texture catalogue

struct TexEntry {
  std::string name;      // mcm2fix name (file name without .png)
  std::string archive;   // e.g. OBJECTS.RES
  std::string member;    // e.g. Antenna01.tga
  std::string how;       // "name" or "stream"
  std::string format;    // e.g. "4:4:4:4, mipmaps"
  std::string file;      // path relative to the output folder
  std::string pal;
  int w = 0, h = 0;
  bool alpha = false;
  uint64_t hash = 0;     // of the pixels, to tell same-name copies apart
};
std::vector<TexEntry> g_tex;
std::map<std::string, std::vector<size_t>> g_tex_by_name;  // name -> indexes into g_tex

uint64_t Fnv(const std::vector<uint8_t>& v) {
  uint64_t x = 14695981039346656037ull;
  for (uint8_t b : v) {
    x ^= b;
    x *= 1099511628211ull;
  }
  return x;
}

struct Counts {
  unsigned textures = 0, images = 0, failed = 0, files = 0, models = 0, tracks = 0;
} g_count;

std::string g_out;  // output folder

// Stores a decoded texture under textures\<name>.png (or in textures\_same_name
// if another picture already has that name).
void AddTexture(TexEntry e, const Image& im) {
  e.w = im.w;
  e.h = im.h;
  e.hash = Fnv(im.rgba);
  auto& list = g_tex_by_name[e.name];
  for (size_t i : list)
    if (g_tex[i].hash == e.hash) return;  // the same picture again (e.g. the same sky twice)
  if (list.empty()) {
    e.file = Join("textures", SafeFile(e.name) + ".png");
  } else {
    std::string dir = Join(Join("textures", "_same_name"), SafeFile(e.archive));
    MakeDirs(Join(g_out, dir));
    e.file = Join(dir, SafeFile(e.name) + ".png");
    // Move the first one there too, so every copy sits side by side.
    TexEntry& first = g_tex[list[0]];
    if (first.file.find("_same_name") == std::string::npos) {
      std::string d2 = Join(Join("textures", "_same_name"), SafeFile(first.archive));
      MakeDirs(Join(g_out, d2));
      std::string nf = Join(d2, SafeFile(first.name) + ".png");
      std::vector<uint8_t> buf;
      if (ReadFile(Join(g_out, first.file), buf) && WriteFile(Join(g_out, nf), buf.data(), buf.size())) {
        std::remove(Join(g_out, first.file).c_str());
        first.file = nf;
      }
    }
    Log("  same name, different picture: %s (%s and %s)", e.name.c_str(), first.archive.c_str(), e.archive.c_str());
  }
  if (!SavePng(Join(g_out, e.file), im)) {
    Log("  could not write %s", e.file.c_str());
    ++g_count.failed;
    return;
  }
  list.push_back(g_tex.size());
  g_tex.push_back(e);
  ++g_count.textures;
}

// Decodes the largest level of a mip-mapped picture with its table at p.
bool DecodeMipped(const uint8_t* d, size_t n, size_t p, uint32_t fmt, int w, int h, const std::string& pal,
                  Image& im, std::string& err) {
  const int bpp = FormatBpp(fmt);
  const Layout lay = FormatLayout(fmt);
  if (!bpp || lay == kNoLayout) {
    err = "unknown format " + Hex(fmt);
    return false;
  }
  std::vector<size_t> offs;
  int k = MipTable(d, n, p, bpp, offs);
  if (!k) {
    err = "bad mip table";
    return false;
  }
  int levels = k - 1;
  if (w <= 0 || h <= 0) w = h = 1 << (levels - 1);
  const size_t a = offs[k - 2], len = offs[k - 1] - offs[k - 2];
  const size_t want = static_cast<size_t>(w) * h * bpp;
  std::vector<uint8_t> px;
  if (!DecodeLevel(d + a, len, want, px)) {
    err = "could not unpack";
    return false;
  }
  Convert(lay, px.data(), w, h, pal, im);
  return true;
}

// A "<name>.tga" member of a RES/TEX: format, x, w, h, palette name, data.
bool DecodeRecord(const std::vector<uint8_t>& d, uint32_t& fmt, size_t& table, std::string& pal, Image& im,
                  std::string& err) {
  if (d.size() < 24) {
    err = "too short";
    return false;
  }
  fmt = U32(&d[0]);
  const uint32_t x = U32(&d[4]);
  const int w = static_cast<int>(U32(&d[8])), h = static_cast<int>(U32(&d[12]));
  const uint32_t nl = U32(&d[16]);
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || nl > 260 || 20 + nl > d.size()) {
    err = "bad header";
    return false;
  }
  pal.assign(reinterpret_cast<const char*>(&d[20]), strnlen(reinterpret_cast<const char*>(&d[20]), nl));
  const size_t q = 20 + nl;
  table = q;
  if (FormatMips(fmt)) return DecodeMipped(d.data(), d.size(), q, fmt, w, h, pal, im, err);
  const int bpp = FormatBpp(fmt);
  const Layout lay = FormatLayout(fmt);
  if (!bpp || lay == kNoLayout) {
    err = "unknown format " + Hex(fmt);
    return false;
  }
  const size_t want = static_cast<size_t>(w) * h * bpp;
  std::vector<uint8_t> px;
  if (x == want && q + 4 + want <= d.size()) {
    px.assign(d.begin() + q + 4, d.begin() + q + 4 + want);
  } else if (x == want && q + want <= d.size()) {
    px.assign(d.begin() + q, d.begin() + q + want);
  } else if (!Lzw(&d[q], d.size() - q, want, px)) {
    err = "could not unpack";
    return false;
  }
  Convert(lay, px.data(), w, h, pal, im);
  return true;
}

std::string FormatText(uint32_t fmt) {
  std::string s = LayoutName(FormatLayout(fmt));
  if (FormatMips(fmt)) s += ", mipmaps";
  return s + " (" + Hex(fmt) + ")";
}

// Bare "u32 format, mip table" records anywhere in a member (sky boxes,
// terrain textures): named <member>_<offset of the table in the archive>.
void ScanEmbedded(const Archive& ar, const Member& m, const std::vector<uint8_t>& d, std::vector<std::string>* names) {
  unsigned found = 0, bad = 0;
  const std::string stem = TextureKey(m.name);
  for (size_t p = 4; p + 16 <= d.size();) {
    const uint32_t fmt = U32(&d[p - 4]);
    const int bpp = FormatBpp(fmt);
    std::vector<size_t> offs;
    int k = (bpp && FormatLayout(fmt) != kNoLayout) ? MipTable(d.data(), d.size(), p, bpp, offs) : 0;
    if (k < 4) {
      ++p;
      continue;
    }
    // Terrain records carry width/height before the format.
    int w = 0, h = 0;
    if (p >= 12) {
      int ww = static_cast<int>(U32(&d[p - 12])), hh = static_cast<int>(U32(&d[p - 8]));
      if (ww == hh && ww == (1 << (k - 2))) w = ww, h = hh;
    }
    Image im;
    std::string err;
    TexEntry e;
    e.name = stem + "_" + Hex(m.offset + static_cast<uint32_t>(p));
    e.archive = ar.name();
    e.member = m.name;
    e.how = "stream";
    e.format = FormatText(fmt);
    e.alpha = LayoutAlpha(FormatLayout(fmt));
    if (DecodeMipped(d.data(), d.size(), p, fmt, w, h, "", im, err)) {
      AddTexture(e, im);
      if (names) names->push_back(e.name);
      ++found;
    } else {
      Log("  %s %s at %x: %s", ar.name().c_str(), m.name.c_str(), static_cast<unsigned>(p), err.c_str());
      ++bad;
      ++g_count.failed;
    }
    p = offs[k - 1];
  }
  Log("  %s %s: %u embedded textures%s", ar.name().c_str(), m.name.c_str(), found,
      bad ? Fmt(", %u failed", bad).c_str() : "");
}

// ---------------------------------------------------------------- models (.slt)

std::vector<std::string> SplitLines(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a <= s.size()) {
    size_t b = s.find('\n', a);
    if (b == std::string::npos) b = s.size();
    std::string l = s.substr(a, b - a);
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    out.push_back(l);
    a = b + 1;
  }
  return out;
}
std::vector<double> Numbers(const std::string& l) {
  std::vector<double> v;
  const char* p = l.c_str();
  while (*p) {
    char* e;
    double x = std::strtod(p, &e);
    if (e == p) {
      ++p;
      continue;
    }
    v.push_back(x);
    p = e;
  }
  return v;
}

struct ModelEntry {
  std::string name, archive, obj, info;
};
std::vector<ModelEntry> g_models;

// Writes LOD 0 of a text model as OBJ + MTL into models\<archive>\.
void ConvertSlt(const Archive& ar, const Member& m, const std::vector<uint8_t>& d) {
  std::string s(d.begin(), d.end());
  std::vector<std::string> lines = SplitLines(s);
  std::vector<std::string> mats;
  std::string section;
  struct Surf {
    std::vector<std::vector<double>> v;
    std::vector<std::vector<long>> f;
    int mat = 0;
  };
  std::map<std::string, Surf> surfs;  // "LOD 0 - Surface 3"
  for (const std::string& l : lines) {
    if (l.empty() || l[0] == ';') continue;
    if (l[0] == '[') {
      section = l.substr(1, l.find(']') == std::string::npos ? std::string::npos : l.find(']') - 1);
      continue;
    }
    if (section.rfind("Material - ", 0) == 0 && l.rfind("TextureMap=", 0) == 0) {
      mats.push_back(l.substr(11));
      continue;
    }
    if (section.rfind("Material - ", 0) == 0 && mats.size() < static_cast<size_t>(std::atoi(section.c_str() + 11)) + 1)
      mats.resize(std::atoi(section.c_str() + 11) + 1);
    if (section.rfind("LOD 0 - Surface ", 0) != 0) continue;
    const size_t dash = section.find(" - ", 16);
    const std::string surf = section.substr(0, dash);
    const std::string part = dash == std::string::npos ? "" : section.substr(dash + 3);
    Surf& S = surfs[surf];
    if (part.empty()) {
      if (l.rfind("Material#0=", 0) == 0) S.mat = std::atoi(l.c_str() + 11);
    } else if (part == "Vertices") {
      std::vector<double> v = Numbers(l);
      if (v.size() >= 8) S.v.push_back(v);
    } else if (part == "Faces") {
      std::vector<double> v = Numbers(l);
      if (v.size() >= 3) S.f.push_back({static_cast<long>(v[0]), static_cast<long>(v[1]), static_cast<long>(v[2])});
    }
  }
  if (surfs.empty()) {
    Log("  %s %s: no LOD 0 surfaces", ar.name().c_str(), m.name.c_str());
    return;
  }
  const std::string dir = Join("models", SafeFile(ar.name()));
  MakeDirs(Join(g_out, dir));
  const std::string stem = SafeFile(StripExt(m.name));
  std::string obj = "# " + m.name + " from " + ar.name() + " (LOD 0), written by mcm2res\n";
  obj += "# The game is left-handed; z is negated here so the model is not mirrored.\n";
  obj += "mtllib " + stem + ".mtl\n";
  std::string mtl;
  for (size_t i = 0; i < mats.size(); ++i) {
    mtl += Fmt("newmtl mat%d\nKd 1 1 1\n", static_cast<int>(i));
    if (!mats[i].empty()) mtl += "map_Kd ../../textures/" + TextureKey(mats[i]) + ".png\n";
    mtl += "\n";
  }
  if (mats.empty()) mtl += "newmtl mat0\nKd 1 1 1\n";
  long base = 1;
  size_t nv = 0, nf = 0;
  for (auto& kv : surfs) {
    Surf& S = kv.second;
    obj += "o " + kv.first + "\nusemtl mat" + std::to_string(S.mat) + "\n";
    for (auto& v : S.v) obj += Fmt("v %.6f %.6f %.6f\n", v[0], v[1], -v[2]);
    for (auto& v : S.v) obj += Fmt("vt %.6f %.6f\n", v[6], 1.0 - v[7]);
    for (auto& v : S.v) obj += Fmt("vn %.6f %.6f %.6f\n", v[3], v[4], -v[5]);
    for (auto& f : S.f) {
      // Negating z flips the winding: swap two corners back.
      long a = f[0] + base, b = f[2] + base, c = f[1] + base;
      obj += Fmt("f %ld/%ld/%ld %ld/%ld/%ld %ld/%ld/%ld\n", a, a, a, b, b, b, c, c, c);
    }
    base += static_cast<long>(S.v.size());
    nv += S.v.size();
    nf += S.f.size();
  }
  WriteText(Join(g_out, Join(dir, stem + ".obj")), obj);
  WriteText(Join(g_out, Join(dir, stem + ".mtl")), mtl);
  ModelEntry e;
  e.name = m.name;
  e.archive = ar.name();
  e.obj = Join(dir, stem + ".obj");
  e.info = Fmt("%zu vertices, %zu triangles", nv, nf) + (mats.empty() ? "" : ", texture " + mats[0]);
  g_models.push_back(e);
  ++g_count.models;
}

// ---------------------------------------------------------------- terrain

struct Ini {
  std::map<std::string, std::map<std::string, std::string>> s;  // lower-case section -> lower-case key -> value
  void Parse(const std::string& text) {
    std::string sec;
    for (std::string l : SplitLines(text)) {
      size_t a = l.find_first_not_of(" \t");
      if (a == std::string::npos) continue;
      l = l.substr(a);
      if (l[0] == ';') continue;
      if (l[0] == '[') {
        sec = Lower(l.substr(1, l.find(']') - 1));
        continue;
      }
      size_t eq = l.find('=');
      if (eq == std::string::npos) continue;
      std::string k = l.substr(0, eq), v = l.substr(eq + 1);
      while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
      while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      s[sec][Lower(k)] = v;
    }
  }
  std::string Get(const std::string& sec, const std::string& key, const std::string& def = "") const {
    auto a = s.find(Lower(sec));
    if (a == s.end()) return def;
    auto b = a->second.find(Lower(key));
    return b == a->second.end() ? def : b->second;
  }
};

struct Race {
  std::string name, file;
  std::vector<std::pair<double, double>> pts;  // x, z
};

struct TrackEntry {
  std::string env, name, event, sky, dir, info;
  std::vector<std::string> pictures;  // relative paths of pictures to show
};
std::vector<TrackEntry> g_tracks;

// A node record (0x47f550).
struct Node {
  float s10 = 1, s14 = 0, ymin = 0, ymax = 0;
  int tile = 0;
  uint32_t tex = 0;
  uint32_t sub[16] = {};
  uint32_t kids = 0;
  int gx = 0, gy = 0, shift = 0, level = 0;
};

class Terrain {
 public:
  bool Parse(const std::vector<uint8_t>& d, std::string& err) {
    d_ = &d;
    if (d.size() < 64 || std::memcmp(d.data(), "TRN", 4) != 0) {
      err = "not a TRN file";
      return false;
    }
    size_t p = 0x18;
    for (int i = 0; i < 3; ++i) {  // palette, two shading tables
      if (p + 4 > d.size()) return Bad(err, "short header");
      p += 4 + U32(&d[p]);
    }
    if (p + 24 > d.size()) return Bad(err, "short header");
    scale_ = F32(&d[p + 4]);
    root_level_ = static_cast<int>(U32(&d[p + 8]));
    root_ = U32(&d[p + 12]);
    const uint32_t nt = U32(&d[p + 16]);
    if (nt > 1000000 || p + 24 + 4ull * nt + 8 > d.size()) return Bad(err, "bad tile table");
    for (uint32_t i = 0; i < nt; ++i) tiles_.push_back(U32(&d[p + 24 + 4 * i]));
    size_t q = p + 24 + 4 * nt;
    const uint32_t ns = U32(&d[q]);
    if (ns > 100000 || q + 8 + 4ull * ns > d.size()) return Bad(err, "bad shared texture table");
    for (uint32_t i = 0; i < ns; ++i) shared_.push_back(U32(&d[q + 8 + 4 * i]));
    if (root_level_ < 0 || root_level_ > 6 || !(scale_ > 0 && scale_ < 1000)) return Bad(err, "bad terrain header");
    Node root;
    if (!ReadNode(root_, root)) return Bad(err, "bad root record");
    root.gx = root.gy = 0;
    root.level = root_level_;
    root.shift = 4 * root_level_;
    Walk(root, 0);
    Log("    terrain: scale %.2f, root level %d, %zu tiles, %zu shared textures, %zu nodes (%s), %u bad records",
        scale_, root_level_, tiles_.size(), shared_.size(), nodes_.size(), LevelCounts().c_str(), bad_);
    return true;
  }
  float scale() const { return scale_; }
  const std::vector<Node>& nodes() const { return nodes_; }
  const std::vector<uint32_t>& shared() const { return shared_; }
  std::string LevelCounts() const {
    std::map<int, int> c;
    for (const Node& n : nodes_) ++c[n.level];
    std::string s;
    for (auto& kv : c) s += Fmt("%slevel %d: %d", s.empty() ? "" : ", ", kv.first, kv.second);
    return s;
  }

  // Heights in feet on the finest grid (one point per level-0 cell corner).
  // Cells without finer nodes are filled bilinearly from their node.
  bool Heights(std::vector<float>& hm, int& W, int& H) {
    int maxx = 0, maxy = 0;
    for (const Node& n : nodes_)
      if (n.level == root_level_ - 1) maxx = std::max(maxx, n.gx + 16), maxy = std::max(maxy, n.gy + 16);
    if (!maxx) return false;
    W = (maxx << (4 * (root_level_ - 1))) + 1;
    H = (maxy << (4 * (root_level_ - 1))) + 1;
    hm.assign(static_cast<size_t>(W) * H, 0.0f);
    for (int lv = root_level_ - 1; lv >= 0; --lv) {
      for (const Node& n : nodes_) {
        if (n.level != lv) continue;
        float g[17 * 17];
        if (!Grid(n, g)) continue;
        const int step = 1 << n.shift;
        const int x0 = n.gx << n.shift, z0 = n.gy << n.shift;
        for (int zz = 0; zz <= 16 * step; ++zz) {
          const int z = z0 + zz;
          if (z >= H) break;
          const int iz = std::min(zz / step, 15);
          const float fz = static_cast<float>(zz - iz * step) / step;
          for (int xx = 0; xx <= 16 * step; ++xx) {
            const int x = x0 + xx;
            if (x >= W) break;
            const int ix = std::min(xx / step, 15);
            const float fx = static_cast<float>(xx - ix * step) / step;
            const float a = g[iz * 17 + ix], b = g[iz * 17 + ix + 1], c = g[(iz + 1) * 17 + ix],
                        e = g[(iz + 1) * 17 + ix + 1];
            hm[static_cast<size_t>(z) * W + x] =
                (a * (1 - fx) + b * fx) * (1 - fz) + (c * (1 - fx) + e * fx) * fz;
          }
        }
      }
    }
    return true;
  }

 private:
  bool Bad(std::string& err, const char* why) {
    err = why;
    return false;
  }
  bool ReadNode(uint32_t off, Node& n) {
    const std::vector<uint8_t>& d = *d_;
    if (off + 97ull > d.size()) return false;
    const uint8_t* p = &d[off];
    n.s10 = F32(p);
    n.s14 = F32(p + 4);
    n.ymin = F32(p + 8);
    n.ymax = F32(p + 12);
    n.tile = U16(p + 18);
    n.tex = U32(p + 20);
    for (int i = 0; i < 16; ++i) n.sub[i] = U32(p + 24 + 4 * i);
    const uint32_t len = U32(p + 88);
    if (len > (16u << 20) || off + 97ull + len > d.size()) return false;
    n.kids = U32(p + 93 + len);
    return std::isfinite(n.s10) && std::isfinite(n.s14);
  }
  void Walk(const Node& n, int depth) {
    nodes_.push_back(n);
    if (!n.kids || n.level == 0 || depth > 8) return;
    const std::vector<uint8_t>& d = *d_;
    if (n.kids + 1024ull > d.size()) {
      ++bad_;
      return;
    }
    for (int i = 0; i < 256; ++i) {
      uint32_t off = U32(&d[n.kids + 4 * i]);
      if (!off) continue;
      Node c;
      if (!ReadNode(off, c)) {
        ++bad_;
        continue;
      }
      c.gx = (n.gx + (i & 15)) * 16;
      c.gy = (n.gy + (i >> 4)) * 16;
      c.shift = n.shift - 4;
      c.level = n.level - 1;
      Walk(c, depth + 1);
    }
  }
  // The node's 17x17 heights in feet.
  bool Grid(const Node& n, float* g) {
    if (n.tile < 0 || static_cast<size_t>(n.tile) >= tiles_.size()) return false;
    auto it = tile_cache_.find(n.tile);
    if (it == tile_cache_.end()) {
      const std::vector<uint8_t>& d = *d_;
      size_t q = tiles_[n.tile];
      std::vector<uint8_t> pts;
      bool ok = true;
      for (int part = 0; part < 3 && ok; ++part) {
        if (q + 8 > d.size()) {
          ok = false;
          break;
        }
        const uint32_t cn = U32(&d[q]), raw = U32(&d[q + 4]);
        if (cn > d.size() - q - 8 || raw > 65536) {
          ok = false;
          break;
        }
        if (part == 2) {
          ok = DecodeLevel(&d[q + 8], cn, raw, pts) && raw >= 289 * 8;
        }
        q += 8 + cn;
      }
      Tile t;
      if (ok && q + 4 <= d.size()) {
        t.base = static_cast<int32_t>(U32(&d[q]));
        for (int i = 0; i < 289; ++i) t.h[i] = U16(&pts[i * 8 + 2]);
        t.ok = true;
      } else {
        ++bad_tiles_;
      }
      it = tile_cache_.emplace(n.tile, t).first;
    }
    const Tile& t = it->second;
    if (!t.ok) return false;
    for (int i = 0; i < 289; ++i) {
      double y = static_cast<double>(static_cast<int32_t>(t.h[i] - static_cast<uint32_t>(t.base))) * n.s10;
      g[i] = static_cast<float>((y + t.base) * n.s14 * scale_);
    }
    return true;
  }

  struct Tile {
    bool ok = false;
    int32_t base = 0;
    uint16_t h[289] = {};
  };
  const std::vector<uint8_t>* d_ = nullptr;
  float scale_ = 1;
  int root_level_ = 2;
  uint32_t root_ = 0;
  std::vector<uint32_t> tiles_, shared_;
  std::vector<Node> nodes_;
  std::map<int, Tile> tile_cache_;
  unsigned bad_ = 0, bad_tiles_ = 0;
};

// Simple drawing on an RGBA image.
void Plot(Image& im, int x, int y, const uint8_t* c) {
  if (x < 0 || y < 0 || x >= im.w || y >= im.h) return;
  uint8_t* o = &im.rgba[(static_cast<size_t>(y) * im.w + x) * 4];
  o[0] = c[0], o[1] = c[1], o[2] = c[2], o[3] = 255;
}
void Disc(Image& im, double cx, double cy, double r, const uint8_t* c) {
  for (int y = static_cast<int>(cy - r); y <= static_cast<int>(cy + r); ++y)
    for (int x = static_cast<int>(cx - r); x <= static_cast<int>(cx + r); ++x)
      if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) Plot(im, x, y, c);
}
void Line(Image& im, double x0, double y0, double x1, double y1, double r, const uint8_t* c) {
  double len = std::hypot(x1 - x0, y1 - y0);
  int steps = std::max(1, static_cast<int>(len / std::max(0.5, r * 0.5)));
  for (int i = 0; i <= steps; ++i) {
    double t = static_cast<double>(i) / steps;
    Disc(im, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, r, c);
  }
}
// Tiny digits for waypoint numbers (3x5 font).
void Digits(Image& im, int x, int y, int n, int s, const uint8_t* c, const uint8_t* bg) {
  static const char* font[10] = {"111101101101111", "010110010010111", "111001111100111", "111001111001111",
                                 "101101111001001", "111100111001111", "111100111101111", "111001001001001",
                                 "111101111101111", "111101111001111"};
  std::string t = std::to_string(n);
  const int w = static_cast<int>(t.size()) * 4 * s + s, h = 7 * s;
  for (int yy = -s; yy < h - s; ++yy)
    for (int xx = -s; xx < w - s; ++xx) Plot(im, x + xx, y + yy, bg);
  for (size_t k = 0; k < t.size(); ++k) {
    const char* g = font[t[k] - '0'];
    for (int r = 0; r < 5; ++r)
      for (int q = 0; q < 3; ++q)
        if (g[r * 3 + q] == '1')
          for (int a = 0; a < s; ++a)
            for (int b = 0; b < s; ++b) Plot(im, x + static_cast<int>(k) * 4 * s + q * s + b, y + r * s + a, c);
  }
}

int g_obj_step = 8;
bool g_raw_all = false;

void ProcessTrack(const Archive& ar, const std::map<std::string, std::vector<uint8_t>>& texts,
                  const std::map<std::string, const Member*>& members, const std::map<std::string, std::string>& trn_textures_first) {
  (void)trn_textures_first;
  // The scene file says which terrain, sky and races.
  std::string scn_name;
  for (auto& kv : texts)
    if (Ext(kv.first) == "scn") scn_name = kv.first;
  if (scn_name.empty()) return;
  Ini scn;
  const std::vector<uint8_t>& sv = texts.at(scn_name);
  scn.Parse(std::string(sv.begin(), sv.end()));
  TrackEntry t;
  t.env = ar.name();
  t.name = scn.Get("SceneInfo", "SceneName", StripExt(ar.name()));
  t.event = scn.Get("SceneInfo", "EventType");
  t.sky = scn.Get("Environment", "CubeFile", "none");
  const std::string trn_file = scn.Get("Environment", "TerrainFile");
  t.dir = Join("tracks", SafeFile(StripExt(ar.name())));
  MakeDirs(Join(g_out, t.dir));
  std::string info = "Track: " + t.name + "\nFile: " + ar.name() + "\nEvent: " + t.event + "\nSky: " + t.sky + "\n";

  // Races: waypoint files in the archive, named by the scene's race names.
  std::vector<Race> races;
  for (auto& kv : texts) {
    if (Ext(kv.first) != "wpt") continue;
    Ini w;
    w.Parse(std::string(kv.second.begin(), kv.second.end()));
    Race r;
    r.file = kv.first;
    int n = std::atoi(w.Get("Waypoints", "NumWaypoints", "0").c_str());
    for (int i = 1; i <= n; ++i) {
      std::string xs = w.Get("Waypoints", Fmt("Point%dX", i)), zs = w.Get("Waypoints", Fmt("Point%dZ", i));
      if (xs.empty() || zs.empty()) continue;
      r.pts.push_back({std::atof(xs.c_str()), std::atof(zs.c_str())});
    }
    races.push_back(r);
  }
  std::sort(races.begin(), races.end(), [](const Race& a, const Race& b) { return Lower(a.file) < Lower(b.file); });
  for (size_t i = 0; i < races.size(); ++i) {
    races[i].name = scn.Get("SceneInfo", Fmt("WaypointRaceName%d", static_cast<int>(i) + 1), StripExt(races[i].file));
    info += Fmt("Race %d: %s (%s, %zu waypoints)\n", static_cast<int>(i) + 1, races[i].name.c_str(),
                races[i].file.c_str(), races[i].pts.size());
  }

  // Terrain.
  const Member* tm = nullptr;
  for (auto& kv : members)
    if (Lower(kv.first) == Lower(trn_file) || (trn_file.empty() && Ext(kv.first) == "trn")) tm = kv.second;
  if (!tm) {
    Log("  %s: no terrain file %s", ar.name().c_str(), trn_file.c_str());
    t.info = info;
    WriteText(Join(g_out, Join(t.dir, "info.txt")), info);
    g_tracks.push_back(t);
    return;
  }
  std::vector<uint8_t> d;
  ar.ReadMember(*tm, d);
  Terrain T;
  std::string err;
  if (!T.Parse(d, err)) {
    Log("  %s %s: %s", ar.name().c_str(), tm->name.c_str(), err.c_str());
    ++g_count.failed;
    t.info = info;
    g_tracks.push_back(t);
    return;
  }
  std::vector<float> hm;
  int W = 0, H = 0;
  if (!T.Heights(hm, W, H)) {
    Log("  %s: no terrain nodes", ar.name().c_str());
    g_tracks.push_back(t);
    return;
  }
  const float sc = T.scale();
  float lo = 1e30f, hi = -1e30f;
  for (float v : hm) lo = std::min(lo, v), hi = std::max(hi, v);
  if (hi <= lo) hi = lo + 1;
  info += Fmt("Terrain: %s, %d x %d points, %.2f ft apart (%.0f x %.0f ft), heights %.1f to %.1f ft\n",
              tm->name.c_str(), W, H, sc, (W - 1) * sc, (H - 1) * sc, lo, hi);
  info += "Nodes: " + T.LevelCounts() + "\n";

  // Height map: 16-bit PNG and raw .r16 (little-endian), 0 = lowest, 65535 = highest.
  {
    std::vector<uint16_t> v(hm.size());
    for (size_t i = 0; i < hm.size(); ++i)
      v[i] = static_cast<uint16_t>(std::lround((hm[i] - lo) / (hi - lo) * 65535.0));
    SavePng16(Join(g_out, Join(t.dir, "heightmap16.png")), W, H, v);
    if (g_raw_all) {
      std::vector<uint8_t> raw(v.size() * 2);
      for (size_t i = 0; i < v.size(); ++i) raw[2 * i] = static_cast<uint8_t>(v[i]), raw[2 * i + 1] = static_cast<uint8_t>(v[i] >> 8);
      WriteFile(Join(g_out, Join(t.dir, "heightmap.r16")), raw.data(), raw.size());
    }
    info += Fmt("heightmap16.png: %d x %d, 16-bit grey, 0 = %.2f ft, 65535 = %.2f ft (row 0 = z 0, column 0 = x 0)%s\n",
                W, H, lo, hi, g_raw_all ? "; heightmap.r16 is the same as raw little-endian 16-bit" : "");
  }

  // Ground: each node's own texture where it has one (the coarse levels
  // first, finer ones over them), as the game draws it close up.
  Image ground;
  ground.w = W - 1;
  ground.h = H - 1;
  ground.rgba.assign(static_cast<size_t>(ground.w) * ground.h * 4, 0);
  unsigned placed = 0, skipped_streamed = 0, skipped_subs = 0;
  std::map<uint32_t, Image> cache;
  for (int lv = 6; lv >= 0; --lv) {
    for (const Node& n : T.nodes()) {
      if (n.level != lv || n.level > 3) continue;
      const uint32_t code = n.tex & 0xffff;
      if ((code & 0xfffe) == 0xfffe) {
        bool any = false;
        for (uint32_t s : n.sub)
          if ((s & 0xfffe) != 0xfffe) any = true;
        if (any) ++skipped_subs;
        continue;
      }
      if (code & 1) {
        ++skipped_streamed;
        continue;
      }
      const int si = static_cast<int16_t>(code) >> 1;
      if (si < 0 || static_cast<size_t>(si) >= T.shared().size()) continue;
      auto it = cache.find(static_cast<uint32_t>(si));
      if (it == cache.end()) {
        Image im;
        const size_t off = T.shared()[si];
        if (off + 12 <= d.size()) {
          const int w = static_cast<int>(U32(&d[off])), h = static_cast<int>(U32(&d[off + 4]));
          std::string e2;
          if (!DecodeMipped(d.data(), d.size(), off + 12, U32(&d[off + 8]), w, h, "", im, e2)) im = Image();
        }
        it = cache.emplace(static_cast<uint32_t>(si), im).first;
      }
      const Image& tx = it->second;
      if (!tx.w) continue;
      const int span = 16 << n.shift;           // units the node covers
      const int rep = tx.w == 64 ? (1 << n.shift) : span;  // units one copy of the texture covers
      const int x0 = n.gx << n.shift, z0 = n.gy << n.shift;
      for (int z = 0; z < span; ++z) {
        const int gz = z0 + z;
        if (gz >= ground.h) break;
        const double v = ((z % rep) + 0.5) / rep;
        const int ty = std::min(tx.h - 1, static_cast<int>((v * 0.9375 + 0.03125) * tx.h));
        for (int x = 0; x < span; ++x) {
          const int gx = x0 + x;
          if (gx >= ground.w) break;
          const double u = ((x % rep) + 0.5) / rep;
          const int tx_ = std::min(tx.w - 1, static_cast<int>((u * 0.9375 + 0.03125) * tx.w));
          std::memcpy(&ground.rgba[(static_cast<size_t>(gz) * ground.w + gx) * 4],
                      &tx.rgba[(static_cast<size_t>(ty) * tx.w + tx_) * 4], 4);
          ground.rgba[(static_cast<size_t>(gz) * ground.w + gx) * 4 + 3] = 255;
        }
      }
      ++placed;
    }
  }
  Log("    ground: %u node textures placed, %u nodes with streamed textures and %u with sub-block textures not drawn",
      placed, skipped_streamed, skipped_subs);
  // Units without a texture: grey.
  for (size_t i = 0; i < ground.rgba.size(); i += 4)
    if (!ground.rgba[i + 3]) ground.rgba[i] = ground.rgba[i + 1] = ground.rgba[i + 2] = 96, ground.rgba[i + 3] = 255;
  SavePng(Join(g_out, Join(t.dir, "ground.png")), ground);
  info += Fmt("ground.png: %d x %d, the terrain texture as the game lays it (1 pixel = %.2f ft)\n", ground.w, ground.h, sc);

  // Relief: height colours with hill shading (light from the north-west).
  Image relief;
  relief.w = W;
  relief.h = H;
  relief.rgba.resize(static_cast<size_t>(W) * H * 4);
  for (int z = 0; z < H; ++z)
    for (int x = 0; x < W; ++x) {
      auto at = [&](int xx, int zz) {
        xx = std::max(0, std::min(W - 1, xx));
        zz = std::max(0, std::min(H - 1, zz));
        return hm[static_cast<size_t>(zz) * W + xx];
      };
      const double dx = (at(x + 1, z) - at(x - 1, z)) / (2 * sc), dz = (at(x, z + 1) - at(x, z - 1)) / (2 * sc);
      const double nx = -dx, ny = 1, nz = -dz, nl = std::sqrt(nx * nx + ny * ny + nz * nz);
      const double lx = -0.5, ly = 0.7, lz = -0.5, ll = std::sqrt(lx * lx + ly * ly + lz * lz);
      double shade = std::max(0.0, (nx * lx + ny * ly + nz * lz) / (nl * ll));
      const double e = (at(x, z) - lo) / (hi - lo);
      // low green -> tan -> brown -> white
      double r, g, b;
      if (e < 0.33) r = 90 + 300 * e, g = 130 + 150 * e, b = 70 + 60 * e;
      else if (e < 0.66) r = 190 - 60 * (e - 0.33), g = 180 - 120 * (e - 0.33), b = 90 - 30 * (e - 0.33);
      else r = 170 + 240 * (e - 0.66), g = 140 + 330 * (e - 0.66), b = 80 + 500 * (e - 0.66);
      const double k = 0.35 + 0.75 * shade;
      uint8_t* o = &relief.rgba[(static_cast<size_t>(z) * W + x) * 4];
      o[0] = static_cast<uint8_t>(std::min(255.0, r * k));
      o[1] = static_cast<uint8_t>(std::min(255.0, g * k));
      o[2] = static_cast<uint8_t>(std::min(255.0, b * k));
      o[3] = 255;
    }
  SavePng(Join(g_out, Join(t.dir, "relief.png")), relief);
  info += "relief.png: heights as colours with hill shading\n";

  // Race maps: ground picture with each race's waypoints.
  static const uint8_t cols[6][3] = {{255, 60, 40}, {255, 230, 40}, {60, 220, 255}, {120, 255, 90}, {255, 120, 255}, {255, 160, 40}};
  const uint8_t white[3] = {255, 255, 255}, black[3] = {0, 0, 0};
  const int ds = std::max(1, ground.w / 600);
  for (size_t r = 0; r < races.size(); ++r) {
    Image map = ground;
    const uint8_t* c = cols[r % 6];
    const auto& p = races[r].pts;
    for (size_t i = 0; i < p.size(); ++i) {
      const auto& a = p[i];
      const auto& b = p[(i + 1) % p.size()];
      Line(map, a.first / sc, a.second / sc, b.first / sc, b.second / sc, 1.2 * ds, c);
    }
    for (size_t i = 0; i < p.size(); ++i) {
      Disc(map, p[i].first / sc, p[i].second / sc, 4.0 * ds, i == 0 ? white : c);
      Digits(map, static_cast<int>(p[i].first / sc + 6 * ds), static_cast<int>(p[i].second / sc - 3 * ds),
             static_cast<int>(i) + 1, ds, white, black);
    }
    std::string f = Fmt("map_race%d.png", static_cast<int>(r) + 1);
    SavePng(Join(g_out, Join(t.dir, f)), map);
    t.pictures.push_back(Join(t.dir, f));
  }
  if (races.empty()) t.pictures.push_back(Join(t.dir, "ground.png"));
  t.pictures.push_back(Join(t.dir, "relief.png"));
  if (!races.empty())
    info += "map_raceN.png: ground with race N's waypoints in order (white dot = waypoint 1)\n";

  // Terrain mesh (OBJ), every g_obj_step points, textured with ground.png.
  {
    const int st = std::max(1, g_obj_step);
    const int nx = (W - 1) / st + 1, nz = (H - 1) / st + 1;
    std::string obj = "# " + t.name + " terrain from " + ar.name() + ", written by mcm2res\n";
    obj += Fmt("# every %d points (%.2f ft); feet; the game is left-handed, so z is negated here\n", st, st * sc);
    obj += "mtllib terrain.mtl\nusemtl ground\n";
    obj.reserve(static_cast<size_t>(nx) * nz * 64);
    for (int z = 0; z < nz; ++z)
      for (int x = 0; x < nx; ++x) {
        const int gx = std::min(W - 1, x * st), gz = std::min(H - 1, z * st);
        obj += Fmt("v %.1f %.2f %.1f\n", gx * sc, hm[static_cast<size_t>(gz) * W + gx], -gz * sc);
      }
    for (int z = 0; z < nz; ++z)
      for (int x = 0; x < nx; ++x) {
        const int gx = std::min(W - 1, x * st), gz = std::min(H - 1, z * st);
        obj += Fmt("vt %.4f %.4f\n", static_cast<double>(gx) / (W - 1), 1.0 - static_cast<double>(gz) / (H - 1));
      }
    for (int z = 0; z + 1 < nz; ++z)
      for (int x = 0; x + 1 < nx; ++x) {
        const long a = static_cast<long>(z) * nx + x + 1, b = a + 1, c = a + nx, e = c + 1;
        obj += Fmt("f %ld/%ld %ld/%ld %ld/%ld\nf %ld/%ld %ld/%ld %ld/%ld\n", a, a, b, b, c, c, b, b, e, e, c, c);
      }
    WriteText(Join(g_out, Join(t.dir, "terrain.obj")), obj);
    WriteText(Join(g_out, Join(t.dir, "terrain.mtl")), "newmtl ground\nKd 1 1 1\nmap_Kd ground.png\n");
    info += Fmt("terrain.obj: %d x %d vertices (every %d points; --obj-step changes it), textured with ground.png\n", nx, nz, st);
  }
  // Waypoints as text too.
  for (size_t r = 0; r < races.size(); ++r) {
    info += Fmt("\nRace %d waypoints (x, z in feet; height from the terrain):\n", static_cast<int>(r) + 1);
    for (size_t i = 0; i < races[r].pts.size(); ++i) {
      const double x = races[r].pts[i].first, z = races[r].pts[i].second;
      const int gx = std::max(0, std::min(W - 1, static_cast<int>(std::lround(x / sc))));
      const int gz = std::max(0, std::min(H - 1, static_cast<int>(std::lround(z / sc))));
      info += Fmt("  %2zu: %9.2f, %9.2f  (ground %.1f ft)\n", i + 1, x, z, hm[static_cast<size_t>(gz) * W + gx]);
    }
  }
  t.info = info;
  WriteText(Join(g_out, Join(t.dir, "info.txt")), info);
  g_tracks.push_back(t);
  ++g_count.tracks;
}

// ---------------------------------------------------------------- archives -> output

struct ArchiveEntry {
  std::string name, rel, kind;
  struct Row {
    std::string member, what, file;
    uint32_t size = 0;
  };
  std::vector<Row> rows;
};
std::vector<ArchiveEntry> g_archives;

bool IsCustomTexture(const std::vector<uint8_t>& d) {
  return d.size() >= 24 && U32(&d[0]) >= 1 && U32(&d[0]) <= 0x22 && FormatLayout(U32(&d[0])) != kNoLayout;
}

void ProcessArchive(const std::string& path, const std::string& rel) {
  Archive ar;
  std::string err;
  if (!ar.Load(path, err)) {
    Say("  %s: %s", rel.c_str(), err.c_str());
    ++g_count.failed;
    return;
  }
  Say("  %s: %zu files", rel.c_str(), ar.members().size());
  Log("  %s: %s, %zu members", rel.c_str(), ar.encrypted() ? "encrypted" : "not encrypted", ar.members().size());
  ArchiveEntry A;
  A.name = ar.name();
  A.rel = rel;
  const std::string ext = Ext(ar.name());
  A.kind = ext == "env" ? "track" : ext == "dat" ? "menu page" : ext == "tex" ? "stadium textures" : "resources";
  const std::string files_dir = Join(Join("archives", SafeFile(ar.name())), "");
  std::map<std::string, std::vector<uint8_t>> texts;
  std::map<std::string, const Member*> members;
  std::vector<uint8_t> d;
  for (const Member& m : ar.members()) {
    members[m.name] = &m;
    ar.ReadMember(m, d);
    const std::string e = Ext(m.name);
    ArchiveEntry::Row row;
    row.member = m.name;
    row.size = m.size;
    bool save_raw = true;
    if (e == "tga") {
      if (IsCustomTexture(d)) {
        // Game texture record.
        save_raw = g_raw_all;
        uint32_t fmt = 0;
        size_t table = 0;
        std::string pal;
        Image im;
        if (DecodeRecord(d, fmt, table, pal, im, err)) {
          TexEntry t;
          t.name = TextureKey(m.name);
          t.archive = ar.name();
          t.member = m.name;
          t.how = "name";
          t.format = FormatText(fmt);
          t.pal = pal;
          t.alpha = LayoutAlpha(FormatLayout(fmt));
          AddTexture(t, im);
          row.what = Fmt("texture %dx%d, %s", im.w, im.h, t.format.c_str());
          row.file = g_tex_by_name[t.name].empty() ? "" : g_tex[g_tex_by_name[t.name].back()].file;
        } else {
          Log("  %s %s: %s (format %x)", ar.name().c_str(), m.name.c_str(), err.c_str(), U32(&d[0]));
          row.what = "texture (could not decode: " + err + ")";
          ++g_count.failed;
          save_raw = true;
        }
      } else {
        // An ordinary TGA picture (track previews, menu art, fonts).
        Image im;
        if (DecodeTga(d.data(), d.size(), im, err)) {
          std::string dir = Join("images", SafeFile(ar.name()));
          MakeDirs(Join(g_out, dir));
          std::string f = Join(dir, SafeFile(StripExt(m.name)) + ".png");
          SavePng(Join(g_out, f), im);
          row.what = Fmt("picture %dx%d (TGA)", im.w, im.h);
          row.file = f;
          ++g_count.images;
        } else {
          row.what = "TGA (" + err + ")";
        }
      }
    } else if (e == "pal8") {
      if (d.size() >= 12) {
        uint32_t nl = U32(&d[8]);
        if (12 + nl + 768 <= d.size()) g_palettes[Lower(m.name)].assign(d.begin() + 12 + nl, d.begin() + 12 + nl + 768);
      }
      row.what = "palette";
    } else if (e == "cub") {
      save_raw = g_raw_all;
      ScanEmbedded(ar, m, d, nullptr);
      row.what = "sky box (48 textures, see the sky group)";
    } else if (e == "trn") {
      save_raw = g_raw_all;
      std::vector<std::string> names;
      ScanEmbedded(ar, m, d, &names);
      row.what = Fmt("terrain (%zu textures; height map and maps in tracks\\)", names.size());
    } else if (e == "slt") {
      ConvertSlt(ar, m, d);
      row.what = "model (text; OBJ in models\\)";
      if (!g_models.empty() && g_models.back().name == m.name) row.file = g_models.back().obj;
    } else if (e == "wav") {
      row.what = "sound";
    } else if (e == "scn" || e == "wpt" || e == "ini" || e == "mcf" || e == "vut" || e == "pb" || e == "seg" ||
               e == "mbf" || e == "vue" || e == "dtm" || e == "txt") {
      row.what = "text";
      texts[m.name] = d;
    } else {
      row.what = "data";
    }
    if (save_raw) {
      MakeDirs(Join(g_out, files_dir));
      std::string f = Join(files_dir, SafeFile(m.name));
      if (WriteFile(Join(g_out, f), d.data(), d.size())) {
        ++g_count.files;
        if (row.file.empty()) row.file = f;
      }
    }
    A.rows.push_back(row);
  }
  if (ext == "env") ProcessTrack(ar, texts, members, {});
  g_archives.push_back(A);
}

// ---------------------------------------------------------------- index.html

void WriteIndex(const std::string& game) {
  std::string h;
  h += "<!doctype html><html><head><meta charset=\"utf-8\"><title>MCM2 resources</title>\n";
  h += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<style>\n"
       ":root{--bg:#16181c;--fg:#e8e8e8;--dim:#9aa0a8;--card:#22252b;--line:#33373f;--acc:#f0a030}\n"
       "body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,Segoe UI,sans-serif}\n"
       "header{padding:16px 20px;border-bottom:1px solid var(--line);position:sticky;top:0;background:var(--bg);z-index:2}\n"
       "h1{font-size:20px;margin:0 0 6px}h2{font-size:17px;margin:28px 0 10px}h3{font-size:15px;margin:18px 0 8px}\n"
       "nav a{color:var(--acc);margin-right:14px;text-decoration:none}a{color:var(--acc)}\n"
       "main{padding:0 20px 40px}.dim{color:var(--dim)}\n"
       "input{background:var(--card);color:var(--fg);border:1px solid var(--line);padding:6px 10px;border-radius:6px;width:260px;margin-top:8px}\n"
       ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px}\n"
       ".t{background:var(--card);border-radius:8px;padding:8px;overflow:hidden}\n"
       ".t img{width:100%;aspect-ratio:1;object-fit:contain;image-rendering:pixelated;border-radius:4px;"
       "background:repeating-conic-gradient(#3a3d44 0 25%,#2c2f35 0 50%) 50%/16px 16px}\n"
       ".t .n{font-family:Consolas,monospace;font-size:12px;word-break:break-all}.t .m{font-size:11px;color:var(--dim)}\n"
       ".tracks{display:grid;grid-template-columns:repeat(auto-fill,minmax(320px,1fr));gap:14px}\n"
       ".track{background:var(--card);border-radius:8px;padding:10px}.track img{width:100%;border-radius:4px}\n"
       ".track .pics{display:flex;gap:6px;overflow-x:auto}.track .pics a{flex:0 0 48%}.track .pics img{width:100%}\n"
       "pre{white-space:pre-wrap;font-size:12px;color:var(--dim);max-height:220px;overflow:auto}\n"
       "table{border-collapse:collapse;width:100%;font-size:13px}td,th{border-bottom:1px solid var(--line);padding:3px 8px;text-align:left}\n"
       "details{background:var(--card);border-radius:8px;padding:6px 10px;margin:6px 0}summary{cursor:pointer}\n"
       "</style></head><body>\n";
  h += "<header><h1>Motocross Madness 2 resources</h1><nav><a href=\"#tracks\">Tracks</a><a href=\"#textures\">Textures</a>"
       "<a href=\"#images\">Pictures</a><a href=\"#models\">Models</a><a href=\"#archives\">Archives</a></nav>"
       "<input id=\"q\" placeholder=\"Filter by name...\" oninput=\"f(this.value)\"></header><main>\n";
  h += "<p class=\"dim\">From " + HtmlEsc(game) + Fmt(". %u textures, %u pictures, %u models, %u tracks, %u files.",
                                                       g_count.textures, g_count.images, g_count.models, g_count.tracks,
                                                       g_count.files) +
       " Texture file names are the names mcm2fix uses: copy a picture into the game's <code>textures\\</code> "
       "folder (or <code>%LOCALAPPDATA%\\Motocross Madness 2\\textures\\</code>), edit it, and the game uses it "
       "(TextureReplace=1).</p>\n";

  // Tracks
  h += "<h2 id=\"tracks\">Tracks</h2><div class=\"tracks\">\n";
  for (const TrackEntry& t : g_tracks) {
    h += "<div class=\"track f\" data-n=\"" + HtmlEsc(Lower(t.name + " " + t.env)) + "\"><h3>" + HtmlEsc(t.name) +
         " <span class=\"dim\">" + HtmlEsc(t.env) + "</span></h3><div class=\"pics\">";
    for (const std::string& p : t.pictures)
      h += "<a href=\"" + Url(p) + "\"><img loading=\"lazy\" src=\"" + Url(p) + "\"></a>";
    h += "</div><pre>" + HtmlEsc(t.info.substr(0, t.info.find("\nRace 1 waypoints"))) + "</pre><a href=\"" +
         Url(t.dir) + "\">folder</a></div>\n";
  }
  h += "</div>\n";

  // Textures by archive
  h += "<h2 id=\"textures\">Textures</h2><p class=\"dim\">Grouped by archive. \"by name\" textures are loaded by "
       "their file name; \"stream\" ones (sky boxes, terrain) by archive position, so their names carry an offset. "
       "Pictures in <code>textures\\_same_name\\</code> share a name with a different picture elsewhere (e.g. every "
       "stadium's texturePanel0): the game can't tell them apart, so a replacement with that name is used for all of them.</p>\n";
  std::map<std::string, std::vector<const TexEntry*>> by_ar;
  for (const TexEntry& t : g_tex) by_ar[t.archive].push_back(&t);
  for (auto& kv : by_ar) {
    h += "<details open><summary><b>" + HtmlEsc(kv.first) + "</b> <span class=\"dim\">" +
         std::to_string(kv.second.size()) + " textures</span></summary><div class=\"grid\">\n";
    for (const TexEntry* t : kv.second) {
      h += "<div class=\"t f\" data-n=\"" + HtmlEsc(Lower(t->name + " " + t->member)) + "\"><a href=\"" + Url(t->file) +
           "\"><img loading=\"lazy\" src=\"" + Url(t->file) + "\"></a><div class=\"n\">" + HtmlEsc(t->name) +
           "</div><div class=\"m\">" + Fmt("%dx%d ", t->w, t->h) + HtmlEsc(t->format) + (t->how == "stream" ? ", stream" : ", by name") +
           "<br>" + HtmlEsc(t->member) + "</div></div>\n";
    }
    h += "</div></details>\n";
  }

  // Pictures (plain TGAs)
  h += "<h2 id=\"images\">Pictures</h2><p class=\"dim\">Plain TGA pictures inside archives (track previews, menu art). "
       "Menus use the game's own files, not texture replacement.</p>\n";
  for (const ArchiveEntry& A : g_archives) {
    std::string cards;
    int n = 0;
    for (const auto& r : A.rows)
      if (r.what.rfind("picture", 0) == 0) {
        cards += "<div class=\"t f\" data-n=\"" + HtmlEsc(Lower(r.member)) + "\"><a href=\"" + Url(r.file) +
                 "\"><img loading=\"lazy\" src=\"" + Url(r.file) + "\"></a><div class=\"n\">" + HtmlEsc(r.member) +
                 "</div><div class=\"m\">" + HtmlEsc(r.what) + "</div></div>\n";
        ++n;
      }
    if (n)
      h += "<details><summary><b>" + HtmlEsc(A.name) + "</b> <span class=\"dim\">" + std::to_string(n) +
           " pictures</span></summary><div class=\"grid\">" + cards + "</div></details>\n";
  }

  // Models
  h += "<h2 id=\"models\">Models</h2><p class=\"dim\">Text models (.slt) as OBJ with their texture. Bike, rider and "
       "object models (.slb) are binary and not converted yet.</p><table><tr><th>Model</th><th>Archive</th><th></th></tr>\n";
  for (const ModelEntry& m : g_models)
    h += "<tr class=\"f\" data-n=\"" + HtmlEsc(Lower(m.name)) + "\"><td><a href=\"" + Url(m.obj) + "\">" + HtmlEsc(m.name) +
         "</a></td><td>" + HtmlEsc(m.archive) + "</td><td class=\"dim\">" + HtmlEsc(m.info) + "</td></tr>\n";
  h += "</table>\n";

  // Archives
  h += "<h2 id=\"archives\">Archives</h2>\n";
  for (const ArchiveEntry& A : g_archives) {
    h += "<details><summary><b>" + HtmlEsc(A.rel) + "</b> <span class=\"dim\">" + HtmlEsc(A.kind) + ", " +
         std::to_string(A.rows.size()) + " files</span></summary><table><tr><th>File</th><th>Bytes</th><th>What</th></tr>\n";
    for (const auto& r : A.rows) {
      h += "<tr class=\"f\" data-n=\"" + HtmlEsc(Lower(r.member)) + "\"><td>" +
           (r.file.empty() ? HtmlEsc(r.member) : "<a href=\"" + Url(r.file) + "\">" + HtmlEsc(r.member) + "</a>") +
           "</td><td>" + std::to_string(r.size) + "</td><td class=\"dim\">" + HtmlEsc(r.what) + "</td></tr>\n";
    }
    h += "</table></details>\n";
  }
  h += "</main><script>function f(q){q=q.toLowerCase();for(const e of document.querySelectorAll('.f'))"
       "e.style.display=!q||e.dataset.n.includes(q)?'':'none';"
       "for(const d of document.querySelectorAll('details'))if(q)d.open=true}</script></body></html>\n";
  WriteText(Join(g_out, "index.html"), h);
}

// ---------------------------------------------------------------- main

bool g_pause = false;
void Pause() {
  if (!g_pause) return;
  std::printf("\nPress Enter to close.");
  std::fflush(stdout);
  std::getchar();
}
[[noreturn]] void Fail(const std::string& msg) {
  std::printf("\nError: %s\n", msg.c_str());
  Pause();
  std::exit(1);
}

bool IsGameDir(const std::string& d) { return !FindIn(d, "RES").empty() && !FindIn(d, "TERAFORM").empty(); }
std::string FindGame(const std::string& hint) {
  std::vector<std::string> tries;
  if (!hint.empty()) tries = {hint, Join(hint, "mcm2"), DirName(hint)};
  std::string exe = ExeDir(), cur = CurDir();
  for (const std::string& d : {exe, cur}) {
    tries.push_back(d);
    tries.push_back(Join(d, "mcm2"));
    tries.push_back(DirName(d));
    tries.push_back(Join(DirName(d), "mcm2"));
    tries.push_back(Join(DirName(DirName(d)), "mcm2"));
  }
  for (const std::string& d : tries)
    if (IsDir(d) && IsGameDir(d)) return d;
  return "";
}

// Every archive in the game folder, as (path, path relative to the game).
std::vector<std::pair<std::string, std::string>> GameArchives(const std::string& game) {
  std::vector<std::pair<std::string, std::string>> out;
  auto add_dir = [&](const std::string& dir, const std::string& rel, std::initializer_list<const char*> exts) {
    if (dir.empty()) return;
    for (const std::string& f : List(dir)) {
      std::string p = Join(dir, f);
      if (IsDir(p)) continue;
      for (const char* e : exts)
        if (Ext(f) == e) out.push_back({p, rel.empty() ? f : rel + "\\" + f});
    }
  };
  std::string res = FindIn(game, "RES"), ui = FindIn(game, "UI"), ter = FindIn(game, "TERAFORM");
  add_dir(res, "RES", {"res", "tex"});
  add_dir(ui, "UI", {"res", "dat"});
  if (!ter.empty())
    for (const std::string& sub : List(ter))
      if (IsDir(Join(ter, sub))) add_dir(Join(ter, sub), "TERAFORM\\" + sub, {"env"});
  return out;
}

std::vector<std::string> g_only;

void Extract(const std::string& game, const std::string& out) {
  g_out = out;
  MakeDirs(g_out);
  MakeDirs(Join(g_out, "textures"));
  g_log = Open(Join(g_out, "mcm2res.log"), "wb");
  std::time_t now = std::time(nullptr);
  Log("mcm2res, %s", std::ctime(&now));
  Say("Game: %s", game.c_str());
  Say("Output: %s", g_out.c_str());
  auto arcs = GameArchives(game);
  // Palettes first (8-bit textures need them), then everything.
  std::sort(arcs.begin(), arcs.end(), [](const auto& a, const auto& b) {
    auto rank = [](const std::string& r) { return Ext(r) == "env" ? 2 : Ext(r) == "dat" ? 1 : 0; };
    if (rank(a.second) != rank(b.second)) return rank(a.second) < rank(b.second);
    return Lower(a.second) < Lower(b.second);
  });
  for (const auto& a : arcs) {
    if (!g_only.empty()) {
      bool want = false;
      for (const std::string& o : g_only)
        if (Lower(a.second).find(Lower(o)) != std::string::npos) want = true;
      if (!want) continue;
    }
    ProcessArchive(a.first, a.second);
  }
  WriteIndex(game);
  // A plain list of every texture name.
  std::string list = "# name\tsize\tformat\tloaded\tarchive\tmember\tfile\n";
  for (const TexEntry& t : g_tex)
    list += t.name + Fmt("\t%dx%d\t", t.w, t.h) + t.format + "\t" + (t.how == "stream" ? "stream" : "by name") + "\t" +
            t.archive + "\t" + t.member + "\t" + t.file + "\n";
  WriteText(Join(g_out, "textures.txt"), list);
  Say("\nDone: %u textures, %u pictures, %u models, %u tracks, %u files%s.", g_count.textures, g_count.images,
      g_count.models, g_count.tracks, g_count.files,
      g_count.failed ? Fmt(", %u problems (see mcm2res.log)", g_count.failed).c_str() : "");
  Say("Open %s in a browser to look through it.", Join(g_out, "index.html").c_str());
  if (g_log) fclose(g_log);
  g_log = nullptr;
}

void ListArchive(const std::string& path) {
  Archive ar;
  std::string err;
  if (!ar.Load(path, err)) {
    std::printf("%s: %s\n", path.c_str(), err.c_str());
    return;
  }
  std::printf("%s (%s, %zu files)\n", path.c_str(), ar.encrypted() ? "encrypted" : "not encrypted", ar.members().size());
  std::vector<uint8_t> d;
  for (const Member& m : ar.members()) {
    std::string what;
    if (Ext(m.name) == "tga") {
      ar.Read(m.offset, std::min<uint32_t>(m.size, 64), d);
      if (IsCustomTexture(d)) what = Fmt("texture %ux%u, ", U32(&d[8]), U32(&d[12])) + FormatText(U32(&d[0]));
      else if (d.size() >= 18) what = Fmt("TGA %ux%u", U16(&d[12]), U16(&d[14]));
    }
    std::printf("  %-32s %10u  @%08x  %s\n", m.name.c_str(), m.size, m.offset, what.c_str());
  }
}

// Compares a folder of PNGs (e.g. mcm2fix's textures_dump) with the names we produce.
void Check(const std::string& folder, const std::string& game) {
  g_out = Join(ExeDir(), "mcm2res_check_tmp");
  // Names only: run a full extract into a scratch folder would be slow, so
  // read the names from an earlier extract if there is one.
  std::string list_file = Join(Join(ExeDir(), "mcm2res_out"), "textures.txt");
  std::vector<uint8_t> buf;
  if (!ReadFile(list_file, buf)) Fail("run mcm2res once first (it writes mcm2res_out\\textures.txt), then check");
  (void)game;
  std::set<std::string> ours;
  for (const std::string& l : SplitLines(std::string(buf.begin(), buf.end())))
    if (!l.empty() && l[0] != '#') ours.insert(l.substr(0, l.find('\t')));
  unsigned match = 0, other = 0, hashes = 0;
  std::vector<std::string> missing;
  for (const std::string& f : List(folder)) {
    if (Ext(f) != "png") continue;
    std::string n = Lower(StripExt(f));
    bool hash = n.size() == 16 && n.find_first_not_of("0123456789abcdef") == std::string::npos;
    if (hash) ++hashes;
    else if (ours.count(n)) ++match;
    else {
      ++other;
      missing.push_back(n);
    }
  }
  std::printf("%s: %u names match mcm2res, %u named pictures mcm2res doesn't know, %u unnamed (hash) pictures\n",
              folder.c_str(), match, other, hashes);
  for (const std::string& n : missing) std::printf("  unknown: %s\n", n.c_str());
}

const char kUsage[] =
    "mcm2res - Motocross Madness 2 resource viewer and extractor (mcm2fix)\n"
    "\n"
    "  mcm2res [extract] [game folder] [-o folder] [--only text]... [--obj-step N] [--raw-all]\n"
    "      Extracts every archive (RES\\*.RES, RES\\*.TEX, UI\\*.DAT, TERAFORM\\*\\*.ENV):\n"
    "      textures as PNG named for mcm2fix's textures folder, pictures, sounds,\n"
    "      text files, tree models as OBJ, and for each track a height map,\n"
    "      ground texture, race maps and a terrain OBJ. Writes index.html to\n"
    "      browse it all (default output: mcm2res_out\\ next to mcm2res.exe).\n"
    "      --only baja   only archives whose path contains \"baja\" (repeatable)\n"
    "      --obj-step N  terrain OBJ uses every Nth height point (default 8)\n"
    "      --raw-all     also save texture records, terrain and sky files as they\n"
    "                    are, and a raw .r16 height map\n"
    "  mcm2res list <archive>...\n"
    "      Lists the files inside archives.\n"
    "  mcm2res check <folder>\n"
    "      Compares a folder of PNGs (e.g. mcm2fix's textures_dump) with the\n"
    "      texture names from the last extract.\n"
    "\n"
    "Double-click: extract (finds the game next to or above the tool).\n"
    "Drop the game folder on it: extract from that folder.\n"
    "Drop an archive on it: list it.\n";

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
#ifdef _WIN32
  (void)argv;
  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  for (int i = 1; i < wargc; ++i) args.push_back(Utf8(wargv[i]));
  LocalFree(wargv);
  DWORD procs[4];
  g_pause = GetConsoleProcessList(procs, 4) <= 1;  // own console: started from Explorer
#else
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
#endif
  std::string mode, out, game;
  std::vector<std::string> inputs;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "-h" || a == "--help" || a == "/?") {
      std::printf("%s", kUsage);
      return 0;
    } else if (a == "-o" && i + 1 < args.size()) {
      out = args[++i];
    } else if (a == "--obj-step" && i + 1 < args.size()) {
      g_obj_step = std::max(1, std::atoi(args[++i].c_str()));
    } else if (a == "--only" && i + 1 < args.size()) {
      g_only.push_back(args[++i]);
    } else if (a == "--raw-all") {
      g_raw_all = true;
    } else if (mode.empty() && inputs.empty() && (a == "extract" || a == "list" || a == "check")) {
      mode = a;
    } else {
      inputs.push_back(a);
    }
  }
  if (mode.empty()) {
    // Dropped archives: list them; a dropped folder or nothing: extract.
    bool files = !inputs.empty();
    for (const std::string& in : inputs)
      if (IsDir(in)) files = false;
    mode = files ? "list" : "extract";
  }
  if (mode == "list") {
    if (inputs.empty()) Fail("name an archive to list");
    for (const std::string& in : inputs) ListArchive(in);
    Pause();
    return 0;
  }
  if (mode == "check") {
    if (inputs.empty()) Fail("name the folder of PNGs to check");
    Check(inputs[0], inputs.size() > 1 ? inputs[1] : "");
    Pause();
    return 0;
  }
  std::printf("%s\n", "mcm2res - Motocross Madness 2 resource extractor (mcm2fix)");
  if (!g_only.empty()) {
    std::string o;
    for (const std::string& x : g_only) o += (o.empty() ? "" : ", ") + x;
    std::printf("Only archives matching: %s\n", o.c_str());
  }
  game = FindGame(inputs.empty() ? "" : inputs[0]);
  if (game.empty())
    Fail("the game folder was not found. Put mcm2res.exe in (or next to) the game folder, or drop the game folder on it.");
  if (out.empty()) out = Join(ExeDir(), "mcm2res_out");
  Extract(game, out);
  Pause();
  return 0;
}
