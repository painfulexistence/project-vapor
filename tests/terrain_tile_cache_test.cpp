// ============================================================================
// TerrainTileCache unit tests — the baked-tile disk cache ported from
// Atmospheric (same "ATC1" container + 16-bit quantize / left-neighbor delta /
// zigzag varint codec, so tiles baked by either engine replay in the other).
// Everything here is plain file IO + pure codec, no GPU:
//
//   * store -> load round-trips within the 16-bit quantization bound
//   * the header guards (params hash, grid size) reject stale bakes
//   * an empty dir disables the cache; store never overwrites an existing bake
//   * TerrainWorld integration: buildTileGeometry is cache-first — the second
//     build replays from disk (hit counted) and reproduces the synthesized
//     geometry within quantization
// ============================================================================

#include "Vapor/terrain_tile_cache.hpp"
#include "Vapor/terrain_world.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <vector>

using Vapor::TerrainConfig;
using Vapor::TerrainTileCache;
using Vapor::TerrainWorld;

namespace {

// Fresh per-test cache dir under the system temp path, removed on scope exit.
struct TempCacheDir {
    std::filesystem::path path;
    explicit TempCacheDir(const char* tag) {
        path = std::filesystem::temp_directory_path()
            / (std::string("vapor_terrain_cache_test_") + tag);
        std::filesystem::remove_all(path);
    }
    ~TempCacheDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

// Largest error the 16-bit quantizer can introduce on a [0,1] value.
constexpr float kQuantBound = 0.5f / 65535.0f + 1e-6f;

std::vector<float> randomGrid(int w, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(0.0f, 1.0f);
    std::vector<float> g(static_cast<size_t>(w) * w);
    for (float& v : g) v = d(rng);
    return g;
}

}  // namespace

TEST_CASE("hashCombine is FNV-1a with a seeded-continuation form", "[terrain][cache]") {
    // Known FNV-1a vectors (offset basis seeded by h == 0).
    const uint8_t a = 'a';
    CHECK(TerrainTileCache::hashCombine(0, &a, 1) == 0xe40c292cu);
    const uint8_t foobar[] = { 'f', 'o', 'o', 'b', 'a', 'r' };
    CHECK(TerrainTileCache::hashCombine(0, foobar, 6) == 0xbf9cf968u);
    // Chaining bytes equals hashing them in one call.
    const uint32_t h1 = TerrainTileCache::hashCombine(0, foobar, 3);
    CHECK(TerrainTileCache::hashCombine(h1, foobar + 3, 3) == 0xbf9cf968u);
    // Different inputs diverge.
    CHECK(TerrainTileCache::hashCombine(0, foobar, 6)
          != TerrainTileCache::hashCombine(0, foobar, 5));
}

TEST_CASE("store/load round-trips within the quantization bound", "[terrain][cache]") {
    TempCacheDir dir("roundtrip");
    TerrainTileCache cache(dir.str());
    REQUIRE(cache.enabled());

    const int w = 67;
    const uint32_t hash = 0xDEADBEEFu;

    // Worst case for the delta coder: uncorrelated noise (multi-byte varints).
    const auto noisy = randomGrid(w, 42u);
    cache.store(3, -2, 1, hash, w, noisy);
    std::vector<float> back;
    REQUIRE(cache.load(3, -2, 1, hash, w, back));
    REQUIRE(back.size() == noisy.size());
    for (size_t i = 0; i < noisy.size(); i++) {
        REQUIRE_THAT(back[i], Catch::Matchers::WithinAbs(noisy[i], kQuantBound));
    }

    // Smooth ramp (the real workload): round-trips AND compresses well below
    // the raw 2 bytes/sample of the quantized grid.
    std::vector<float> smooth(static_cast<size_t>(w) * w);
    for (int j = 0; j < w; j++) {
        for (int i = 0; i < w; i++) {
            smooth[static_cast<size_t>(j) * w + i] =
                0.5f + 0.4f * std::sin(i * 0.11f) * std::cos(j * 0.07f);
        }
    }
    cache.store(0, 0, 2, hash, w, smooth);
    REQUIRE(cache.load(0, 0, 2, hash, w, back));
    for (size_t i = 0; i < smooth.size(); i++) {
        REQUIRE_THAT(back[i], Catch::Matchers::WithinAbs(smooth[i], kQuantBound));
    }
    size_t fileSize = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir.path)) {
        if (e.path().filename().string().find("_l2_") != std::string::npos)
            fileSize = static_cast<size_t>(e.file_size());
    }
    REQUIRE(fileSize > 0);
    CHECK(fileSize < static_cast<size_t>(w) * w * 2);

    // Loads are keyed on every coordinate: a different tile/lod misses.
    CHECK_FALSE(cache.load(4, -2, 1, hash, w, back));
    CHECK_FALSE(cache.load(3, -2, 0, hash, w, back));
}

TEST_CASE("header guards reject stale or mismatched bakes", "[terrain][cache]") {
    TempCacheDir dir("guards");
    TerrainTileCache cache(dir.str());
    const int w = 19;
    const auto grid = randomGrid(w, 7u);
    cache.store(1, 1, 0, 0x1111u, w, grid);

    std::vector<float> back;
    // Wrong params hash (changed noise settings) => miss, never a stale replay.
    CHECK_FALSE(cache.load(1, 1, 0, 0x2222u, w, back));
    // Wrong grid size => miss (the header width/height must match).
    CHECK_FALSE(cache.load(1, 1, 0, 0x1111u, w + 2, back));
    CHECK(cache.load(1, 1, 0, 0x1111u, w, back));

    // Store never overwrites an existing bake (first writer wins — concurrent
    // stores of the same tile are benign).
    const auto other = randomGrid(w, 8u);
    cache.store(1, 1, 0, 0x1111u, w, other);
    REQUIRE(cache.load(1, 1, 0, 0x1111u, w, back));
    REQUIRE_THAT(back[0], Catch::Matchers::WithinAbs(grid[0], kQuantBound));
}

TEST_CASE("an empty dir disables the cache", "[terrain][cache]") {
    TerrainTileCache cache("");
    CHECK_FALSE(cache.enabled());
    const int w = 5;
    const auto grid = randomGrid(w, 1u);
    cache.store(0, 0, 0, 1u, w, grid);// no-op
    std::vector<float> back;
    CHECK_FALSE(cache.load(0, 0, 0, 1u, w, back));
}

TEST_CASE("TerrainWorld builds tiles cache-first", "[terrain][cache]") {
    TempCacheDir dir("world");

    TerrainConfig cfg;
    cfg.worldSize = 512.0f;
    cfg.tileSize = 64.0f;
    cfg.heightScale = 120.0f;
    cfg.noiseFrequency = 0.01f;
    cfg.noiseOctaves = 5;
    cfg.seed = 1234u;

    // Reference geometry from a cache-less world (pure synthesis).
    TerrainWorld pure;
    pure.configure(cfg);
    std::vector<Vapor::VertexData> refVerts;
    std::vector<Uint32> refInds;
    glm::vec3 mn, mx;
    pure.buildTileGeometry(2, 5, 1, refVerts, refInds, mn, mx);

    cfg.cacheDir = dir.str();
    TerrainWorld world;
    world.configure(cfg);
    CHECK(world.stats().cacheHits == 0);
    CHECK(world.stats().cacheMisses == 0);

    // First build synthesizes + stores (miss), second replays (hit).
    std::vector<Vapor::VertexData> verts;
    std::vector<Uint32> inds;
    world.buildTileGeometry(2, 5, 1, verts, inds, mn, mx);
    CHECK(world.stats().cacheMisses == 1);
    CHECK(world.stats().cacheHits == 0);
    world.buildTileGeometry(2, 5, 1, verts, inds, mn, mx);
    CHECK(world.stats().cacheHits == 1);

    // The replayed tile reproduces the synthesized geometry within the 16-bit
    // quantization bound (positions/heights; indices are exactly equal).
    REQUIRE(verts.size() == refVerts.size());
    REQUIRE(inds == refInds);
    const float yBound = kQuantBound * cfg.heightScale;
    for (size_t i = 0; i < verts.size(); i++) {
        CHECK(verts[i].position.x == refVerts[i].position.x);
        CHECK(verts[i].position.z == refVerts[i].position.z);
        REQUIRE_THAT(verts[i].position.y,
                     Catch::Matchers::WithinAbs(refVerts[i].position.y, yBound + 1e-4f));
    }

    // A fresh world over the same dir + params boots straight from IO.
    TerrainWorld rebooted;
    rebooted.configure(cfg);
    rebooted.buildTileGeometry(2, 5, 1, verts, inds, mn, mx);
    CHECK(rebooted.stats().cacheHits == 1);
    CHECK(rebooted.stats().cacheMisses == 0);

    // Changing a height-shaping parameter changes the hash: no stale replay.
    TerrainConfig changed = cfg;
    changed.seed = 4321u;
    TerrainWorld other;
    other.configure(changed);
    other.buildTileGeometry(2, 5, 1, verts, inds, mn, mx);
    CHECK(other.stats().cacheMisses == 1);
    CHECK(other.stats().cacheHits == 0);
}
