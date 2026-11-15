// Picture decoding (Wuffs: PNG, TGA, BMP, JPEG) and PNG writing
// (stb_image_write) for texture replacement and TextureDump. Thread safe.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dd {

// Decodes the picture into rows of B, G, R, A bytes (straight alpha).
bool DecodePicture(const std::wstring& path, std::vector<uint8_t>& px, UINT& w, UINT& h, std::string* err);
// Writes B, G, R, A pixels as a PNG file.
bool WritePngFile(const std::wstring& path, const uint8_t* bgra, UINT w, UINT h);

}  // namespace dd
