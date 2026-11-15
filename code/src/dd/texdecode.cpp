// Picture decoding and PNG writing for texture replacement and dumping,
// without the Windows imaging components (WIC): those are not on Windows XP,
// are slow, and changed the game thread's FPU precision.
//
// Decoding: Wuffs (memory-safe, among the fastest PNG decoders), PNG, TGA,
// BMP and JPEG. Writing: stb_image_write (PNG, for TextureDump only).
#include "texdecode.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__AUX__BASE
#define WUFFS_CONFIG__MODULE__AUX__IMAGE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__TARGA
#define WUFFS_CONFIG__MODULE__BMP
#define WUFFS_CONFIG__MODULE__JPEG
#include "../../third_party/wuffs/wuffs-v0.4.c"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBIW_WINDOWS_UTF8
#include "../../third_party/stb/stb_image_write.h"

namespace dd {

namespace {

bool HasExt(const std::wstring& path, const wchar_t* ext) {
  const size_t n = std::wcslen(ext);
  return path.size() >= n && _wcsicmp(path.c_str() + path.size() - n, ext) == 0;
}

class Callbacks : public wuffs_aux::DecodeImageCallbacks {
 public:
  explicit Callbacks(bool tga) : tga_(tga) {}

  wuffs_base__image_decoder::unique_ptr SelectDecoder(uint32_t fourcc, wuffs_base__slice_u8 prefix,
                                                      bool closed) override {
    // TGA has no signature; trust the file name when nothing else matched.
    if (fourcc == 0 && tga_) return wuffs_targa__decoder::alloc_as__wuffs_base__image_decoder();
    return wuffs_aux::DecodeImageCallbacks::SelectDecoder(fourcc, prefix, closed);
  }
  wuffs_base__pixel_format SelectPixfmt(const wuffs_base__image_config&) override {
    // Byte order B, G, R, A, straight alpha: what the texture upload takes.
    return wuffs_base__make_pixel_format(WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL);
  }

 private:
  bool tga_;
};

}  // namespace

bool DecodePicture(const std::wstring& path, std::vector<uint8_t>& px, UINT& w, UINT& h, std::string* err) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) {
    if (err) *err = "could not open the file";
    return false;
  }
  Callbacks cb(HasExt(path, L".tga"));
  wuffs_aux::sync_io::FileInput in(f);
  wuffs_aux::DecodeImageResult res = wuffs_aux::DecodeImage(
      cb, in, wuffs_aux::DecodeImageArgQuirks::DefaultValue(), wuffs_aux::DecodeImageArgFlags::DefaultValue(),
      wuffs_aux::DecodeImageArgPixelBlend(WUFFS_BASE__PIXEL_BLEND__SRC),
      wuffs_aux::DecodeImageArgBackgroundColor::DefaultValue(), wuffs_aux::DecodeImageArgMaxInclDimension(16384));
  std::fclose(f);
  if (!res.error_message.empty() || !res.pixbuf.pixcfg.is_valid()) {
    if (err) *err = res.error_message.empty() ? "not a picture" : res.error_message;
    return false;
  }
  w = res.pixbuf.pixcfg.width();
  h = res.pixbuf.pixcfg.height();
  if (!w || !h) {
    if (err) *err = "empty picture";
    return false;
  }
  const wuffs_base__table_u8 t = res.pixbuf.plane(0);
  px.resize(static_cast<size_t>(w) * h * 4);
  for (UINT y = 0; y < h; ++y)
    std::memcpy(px.data() + static_cast<size_t>(y) * w * 4, t.ptr + static_cast<size_t>(y) * t.stride, w * 4);
  return true;
}

bool WritePngFile(const std::wstring& path, const uint8_t* bgra, UINT w, UINT h) {
  // stb_image_write takes R, G, B, A.
  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < rgba.size(); i += 4) {
    rgba[i] = bgra[i + 2];
    rgba[i + 1] = bgra[i + 1];
    rgba[i + 2] = bgra[i];
    rgba[i + 3] = bgra[i + 3];
  }
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;
  int len = 0;
  unsigned char* png = stbi_write_png_to_mem(rgba.data(), static_cast<int>(w * 4), static_cast<int>(w),
                                             static_cast<int>(h), 4, &len);
  bool ok = png && len > 0 && std::fwrite(png, 1, static_cast<size_t>(len), f) == static_cast<size_t>(len);
  STBIW_FREE(png);
  ok = (std::fclose(f) == 0) && ok;
  if (!ok) DeleteFileW(path.c_str());
  return ok;
}

}  // namespace dd
