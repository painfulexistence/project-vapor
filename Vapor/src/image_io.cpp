#include "image_io.hpp"
#include "stb/stb_image_write.h"
#include <cstring>
#include <fstream>
#include <vector>

namespace Vapor {

// ============================================================================
// Minimal OpenEXR writer.
//
// Emits exactly one file variant: single-part scanline, three FLOAT channels,
// NO_COMPRESSION, increasing-Y. Layout per the OpenEXR file format docs:
//
//   magic(4) version(4)
//   attributes: name\0 type\0 int32(size) payload   ...repeated, then \0
//   line offset table: uint64 absolute offset per scanline
//   per scanline: int32 y, int32 payloadBytes, then each channel's full row
//     in channel-list order.
//
// Two format rules that are easy to get wrong, both covered by the round-trip
// test: channels must be listed alphabetically (B, G, R — not R, G, B), and
// the offset table holds absolute file positions of each scanline block.
// ============================================================================

namespace {

void put32(std::vector<unsigned char>& out, Uint32 v) {
    out.push_back(v & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
}

void put64(std::vector<unsigned char>& out, Uint64 v) {
    for (int i = 0; i < 8; i++) out.push_back((v >> (8 * i)) & 0xFF);
}

void putFloat(std::vector<unsigned char>& out, float f) {
    Uint32 bits;
    std::memcpy(&bits, &f, 4);
    put32(out, bits);
}

void putString(std::vector<unsigned char>& out, const char* s) {
    while (*s) out.push_back(static_cast<unsigned char>(*s++));
    out.push_back(0);
}

// One header attribute: name, type, size, then the caller appends `size`
// payload bytes.
void putAttrHeader(std::vector<unsigned char>& out, const char* name, const char* type, Uint32 size) {
    putString(out, name);
    putString(out, type);
    put32(out, size);
}

} // namespace

bool writeEXR(const std::string& path, const float* rgb, Uint32 width, Uint32 height) {
    if (!rgb || width == 0 || height == 0) return false;

    std::vector<unsigned char> out;
    out.reserve(512 + size_t(width) * height * 12 + size_t(height) * 16);

    // Magic + version 2, no flags (single part, scanline).
    const unsigned char magic[4] = { 0x76, 0x2F, 0x31, 0x01 };
    out.insert(out.end(), magic, magic + 4);
    put32(out, 2);

    // channels: chlist. Alphabetical order is REQUIRED by the format; readers
    // reject or misread R,G,B ordering. Each entry: name\0, pixelType(2=FLOAT),
    // pLinear(0)+3 reserved, xSampling(1), ySampling(1). List ends with \0.
    const char* channelNames[3] = { "B", "G", "R" };
    const Uint32 chlistSize = 3 * (2 + 4 + 4 + 4 + 4) + 1;
    putAttrHeader(out, "channels", "chlist", chlistSize);
    for (const char* name : channelNames) {
        putString(out, name);
        put32(out, 2);  // FLOAT
        put32(out, 0);  // pLinear + reserved[3]
        put32(out, 1);  // xSampling
        put32(out, 1);  // ySampling
    }
    out.push_back(0);

    putAttrHeader(out, "compression", "compression", 1);
    out.push_back(0);  // NO_COMPRESSION

    putAttrHeader(out, "dataWindow", "box2i", 16);
    put32(out, 0); put32(out, 0);
    put32(out, width - 1); put32(out, height - 1);

    putAttrHeader(out, "displayWindow", "box2i", 16);
    put32(out, 0); put32(out, 0);
    put32(out, width - 1); put32(out, height - 1);

    putAttrHeader(out, "lineOrder", "lineOrder", 1);
    out.push_back(0);  // INCREASING_Y

    putAttrHeader(out, "pixelAspectRatio", "float", 4);
    putFloat(out, 1.0f);

    putAttrHeader(out, "screenWindowCenter", "v2f", 8);
    putFloat(out, 0.0f); putFloat(out, 0.0f);

    putAttrHeader(out, "screenWindowWidth", "float", 4);
    putFloat(out, 1.0f);

    out.push_back(0);  // end of header

    // Line offset table: absolute file offset of each scanline block. Every
    // block is the same size here (uncompressed), so offsets are arithmetic.
    const Uint64 tableStart = out.size();
    const Uint64 firstBlock = tableStart + Uint64(height) * 8;
    const Uint64 rowPayload = Uint64(width) * 3 * 4;   // 3 FLOAT channels
    const Uint64 blockSize = 4 + 4 + rowPayload;        // y + size + payload
    for (Uint32 y = 0; y < height; y++) {
        put64(out, firstBlock + Uint64(y) * blockSize);
    }

    // Scanline blocks: y, payload size, then the full row of each channel in
    // channel-list order (B, then G, then R).
    for (Uint32 y = 0; y < height; y++) {
        put32(out, y);
        put32(out, static_cast<Uint32>(rowPayload));
        const float* row = rgb + size_t(y) * width * 3;
        for (int channel : { 2, 1, 0 }) {  // B, G, R from interleaved RGB
            for (Uint32 x = 0; x < width; x++) {
                putFloat(out, row[x * 3 + channel]);
            }
        }
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return file.good();
}

bool writeTonemappedPNG(const std::string& path, const float* rgb, Uint32 width, Uint32 height,
                        float exposure) {
    if (!rgb || width == 0 || height == 0) return false;

    std::vector<unsigned char> pixels(size_t(width) * height * 3);
    for (size_t i = 0; i < size_t(width) * height * 3; i++) {
        const float display = linearToSRGB(acesTonemap(rgb[i] * exposure));
        pixels[i] = static_cast<unsigned char>(display * 255.0f + 0.5f);
    }
    return stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 3,
                          pixels.data(), static_cast<int>(width) * 3) != 0;
}

} // namespace Vapor
