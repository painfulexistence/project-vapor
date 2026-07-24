#include "terrain_tile_cache.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace Vapor {

namespace {

    constexpr uint32_t kMagic = 0x31435441;// "ATC1" — same container as Atmospheric

    struct Header {
        uint32_t magic;
        uint32_t paramsHash;
        uint16_t width;
        uint16_t height;
        uint32_t payloadBytes;
    };

    // Left-neighbor prediction (first column predicts from the row above),
    // zigzag varint residuals. Smooth heightfields produce mostly 1-byte
    // residuals.
    std::vector<uint8_t> encode(const std::vector<float>& grid, int w) {
        std::vector<uint8_t> out;
        out.reserve(static_cast<size_t>(w) * w);
        uint16_t prevRowFirst = 0;
        for (int j = 0; j < w; ++j) {
            uint16_t pred = prevRowFirst;
            for (int i = 0; i < w; ++i) {
                const float clamped = grid[static_cast<size_t>(j) * w + i];
                const uint16_t q = static_cast<uint16_t>(clamped * 65535.0f + 0.5f);
                if (i == 0) prevRowFirst = q;
                const int32_t d = static_cast<int32_t>(q) - static_cast<int32_t>(pred);
                uint32_t z = (static_cast<uint32_t>(d) << 1) ^ static_cast<uint32_t>(d >> 31);
                while (z >= 0x80) {
                    out.push_back(static_cast<uint8_t>(z) | 0x80);
                    z >>= 7;
                }
                out.push_back(static_cast<uint8_t>(z));
                pred = q;
            }
        }
        return out;
    }

    bool decode(const std::vector<uint8_t>& in, int w, std::vector<float>& out) {
        out.resize(static_cast<size_t>(w) * w);
        size_t pos = 0;
        uint16_t prevRowFirst = 0;
        for (int j = 0; j < w; ++j) {
            uint16_t pred = prevRowFirst;
            for (int i = 0; i < w; ++i) {
                uint32_t z = 0;
                int shift = 0;
                while (true) {
                    if (pos >= in.size() || shift > 28) return false;
                    const uint8_t b = in[pos++];
                    z |= static_cast<uint32_t>(b & 0x7F) << shift;
                    if (!(b & 0x80)) break;
                    shift += 7;
                }
                const int32_t d = static_cast<int32_t>(z >> 1) ^ -static_cast<int32_t>(z & 1);
                const uint16_t q = static_cast<uint16_t>(static_cast<int32_t>(pred) + d);
                if (i == 0) prevRowFirst = q;
                out[static_cast<size_t>(j) * w + i] = q / 65535.0f;
                pred = q;
            }
        }
        return pos == in.size();
    }

}// namespace

TerrainTileCache::TerrainTileCache(std::string dirIn) : dir(std::move(dirIn)) {
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) dir.clear();// unwritable target (e.g. read-only bundle) — disable
}

uint32_t TerrainTileCache::hashCombine(uint32_t h, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (h == 0) h = 2166136261u;// FNV offset basis
    for (size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h;
}

std::string TerrainTileCache::pathFor(int tileX, int tileZ, int lod, uint32_t paramsHash) const {
    char name[96];
    std::snprintf(name, sizeof(name), "/t_%08x_l%d_%d_%d.atc", paramsHash, lod, tileX, tileZ);
    return dir + name;
}

bool TerrainTileCache::load(int tileX, int tileZ, int lod, uint32_t paramsHash, int w, std::vector<float>& out) const {
    if (dir.empty()) return false;
    std::ifstream f(pathFor(tileX, tileZ, lod, paramsHash), std::ios::binary);
    if (!f) return false;
    Header h {};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f || h.magic != kMagic || h.paramsHash != paramsHash || h.width != w || h.height != w) return false;
    std::vector<uint8_t> payload(h.payloadBytes);
    f.read(reinterpret_cast<char*>(payload.data()), payload.size());
    if (!f) return false;
    return decode(payload, w, out);
}

void TerrainTileCache::store(
    int tileX, int tileZ, int lod, uint32_t paramsHash, int w, const std::vector<float>& grid
) const {
    if (dir.empty()) return;
    const std::string path = pathFor(tileX, tileZ, lod, paramsHash);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return;

    const std::vector<uint8_t> payload = encode(grid, w);
    Header h {
        kMagic, paramsHash, static_cast<uint16_t>(w), static_cast<uint16_t>(w), static_cast<uint32_t>(payload.size())
    };
    // Write to a per-tile temp name then rename, so concurrent readers never
    // observe a half-written file.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(&h), sizeof(h));
        f.write(reinterpret_cast<const char*>(payload.data()), payload.size());
        if (!f) {
            f.close();
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

}// namespace Vapor
