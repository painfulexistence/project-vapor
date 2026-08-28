#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Vapor {

// Disk cache for streamed terrain tile height grids — a port of Atmospheric's
// TerrainTileCache, keeping the "ATC1" container and codec byte-compatible so
// tiles baked by either engine replay in the other. Ghost-of-Tsushima-style
// loading: baked tiles are read and decoded instead of synthesized, so every
// boot after the first is pure IO. Grids are quantized to 16 bits and
// compressed with a left-neighbor delta + zigzag varint coder —
// dependency-free, decodes at near-memcpy speed, and typically shrinks smooth
// terrain to 25-50% of raw.
//
// Fully portable: plain C++ file IO, safe to call from task-scheduler workers
// (no shared mutable state; concurrent stores of the same tile are benign).
class TerrainTileCache {
public:
    // dir is created on demand; empty (or uncreatable) disables the cache
    // (load misses, store no-ops).
    explicit TerrainTileCache(std::string dir);

    bool enabled() const { return !dir.empty(); }

    // Fills `out` (w*w floats in [0,1]) on hit. paramsHash must match the
    // value used at store time — derive it from every input that shapes the
    // heights (noise params, tile size, cache version).
    bool load(int tileX, int tileZ, int lod, uint32_t paramsHash, int w, std::vector<float>& out) const;

    // Persists a generated grid. Skips silently if the file already exists
    // or the directory isn't writable (e.g. read-only bundles).
    void store(int tileX, int tileZ, int lod, uint32_t paramsHash, int w, const std::vector<float>& grid) const;

    // FNV-1a helper for building parameter hashes.
    static uint32_t hashCombine(uint32_t h, const void* data, size_t size);

private:
    std::string pathFor(int tileX, int tileZ, int lod, uint32_t paramsHash) const;

    std::string dir;
};

}// namespace Vapor
