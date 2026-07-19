// ============================================================================
// TerrainTextureGen unit tests — the procedural detail-layer + splat generator
// ported from Atmospheric (Vapor/terrain_texture_gen.hpp). Pure CPU noise
// synthesis, so every property the terrain shader relies on is checked here:
//
//   * layers are res^2 RGBA8 (albedo + normal), deterministic per (res, seed)
//   * detail maps tile seamlessly — opposite edges agree (periodic noise)
//   * normal maps decode to unit-ish vectors facing +Z (out of the surface)
//   * splat weights are a partition of unity {grass, rock, dirt, snow}, follow
//     the slope/height rules, and are deterministic across a shared border
// ============================================================================

#include "Vapor/terrain_texture_gen.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace Vapor;
using TerrainTextureGen::DetailLayer;

namespace {

void checkRGBA8(const std::shared_ptr<Image>& img, int res, const char* uri) {
    REQUIRE(img);
    CHECK(img->uri == uri);
    CHECK(img->width == static_cast<Uint32>(res));
    CHECK(img->height == static_cast<Uint32>(res));
    CHECK(img->channelCount == 4);
    REQUIRE(img->byteArray.size() == static_cast<size_t>(res) * res * 4);
    // Opaque alpha everywhere.
    for (size_t i = 3; i < img->byteArray.size(); i += 4) CHECK(img->byteArray[i] == 255);
}

// Every row's first vs last texel and every column's first vs last: a
// seamlessly tiling map has opposite edges sampling nearly the same lattice
// value (they wrap to the same period), so the boundary is not a hard seam.
void checkTileable(const std::shared_ptr<Image>& img, int res, int tol) {
    auto at = [&](int x, int y, int c) { return static_cast<int>(img->byteArray[(static_cast<size_t>(y) * res + x) * 4 + c]); };
    int worst = 0;
    for (int y = 0; y < res; ++y)
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(at(0, y, c) - at(res - 1, y, c)));
    for (int x = 0; x < res; ++x)
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(at(x, 0, c) - at(x, res - 1, c)));
    // Wrapping periodic noise means edge N-1 is one lattice step from edge 0,
    // not identical — a small tolerance, but far from a full-range seam.
    CHECK(worst <= tol);
}

}  // namespace

TEST_CASE("detail layers are deterministic res^2 RGBA8 images", "[terraintex]") {
    const int res = 64;
    auto grass = TerrainTextureGen::generateGrass(res, 11);
    checkRGBA8(grass.albedo, res, "terraintexgen_grass_albedo");
    checkRGBA8(grass.normal, res, "terraintexgen_grass_normal");

    // Same seed => byte-identical.
    auto grass2 = TerrainTextureGen::generateGrass(res, 11);
    CHECK(grass.albedo->byteArray == grass2.albedo->byteArray);
    CHECK(grass.normal->byteArray == grass2.normal->byteArray);

    // Different seed => different albedo.
    auto grass3 = TerrainTextureGen::generateGrass(res, 99);
    CHECK(grass.albedo->byteArray != grass3.albedo->byteArray);

    // All four layers produce valid images with their own cache keys.
    checkRGBA8(TerrainTextureGen::generateRock(res, 22).albedo, res, "terraintexgen_rock_albedo");
    checkRGBA8(TerrainTextureGen::generateDirt(res, 33).albedo, res, "terraintexgen_dirt_albedo");
    checkRGBA8(TerrainTextureGen::generateSnow(res, 44).normal, res, "terraintexgen_snow_normal");
}

TEST_CASE("detail albedo maps tile seamlessly", "[terraintex]") {
    const int res = 128;
    // Periodic-noise edges wrap one lattice step apart — continuous, not a hard
    // seam (a full seam would be up to 255).
    checkTileable(TerrainTextureGen::generateGrass(res, 11).albedo, res, 60);
    checkTileable(TerrainTextureGen::generateRock(res, 22).albedo, res, 60);
    checkTileable(TerrainTextureGen::generateSnow(res, 44).albedo, res, 60);
}

TEST_CASE("normal maps decode to unit vectors facing out of the surface", "[terraintex]") {
    const int res = 64;
    auto rock = TerrainTextureGen::generateRock(res, 22);
    const auto& n = rock.normal->byteArray;
    for (size_t i = 0; i < n.size(); i += 4) {
        const float x = n[i + 0] / 255.0f * 2.0f - 1.0f;
        const float y = n[i + 1] / 255.0f * 2.0f - 1.0f;
        const float z = n[i + 2] / 255.0f * 2.0f - 1.0f;
        CHECK(z > 0.0f);  // tangent-space normal points out (+Z)
        CHECK_THAT(std::sqrt(x * x + y * y + z * z), Catch::Matchers::WithinAbs(1.0f, 0.02f));
    }
}

TEST_CASE("defaultSplat weights partition unity and follow slope/height", "[terraintex]") {
    const int res = 48;
    TerrainTextureGen::SplatParams sp;
    sp.heightScale = 500.0f;

    // A ramp rising along +z over a 500 m rect: height 0->1 across 500 m at
    // heightScale 500 gives a rise/run of ~1.0 — well past the rock slope
    // threshold (0.55), so steep faces read as rock.
    auto ramp = [](float /*x*/, float z) { return z / 500.0f; };
    const auto splat = TerrainTextureGen::defaultSplat({ 0.0f, 0.0f }, { 500.0f, 500.0f }, res, ramp, sp);
    REQUIRE(splat.size() == static_cast<size_t>(res) * res * 4);

    for (size_t i = 0; i < splat.size(); i += 4) {
        const int sum = splat[i] + splat[i + 1] + splat[i + 2] + splat[i + 3];
        CHECK(sum >= 250);  // normalized to 1.0 -> ~255 across 4 channels
        CHECK(sum <= 258);
    }

    // Determinism: a tile sharing this world rect regenerates identically
    // (world-space noise => borders agree between neighbours).
    const auto splat2 = TerrainTextureGen::defaultSplat({ 0.0f, 0.0f }, { 500.0f, 500.0f }, res, ramp, sp);
    CHECK(splat == splat2);

    // A steep ramp (high slope everywhere) must produce more rock than a flat
    // world of the same heights.
    auto flat = [](float, float) { return 0.4f; };  // mid height, zero slope
    const auto splatFlat = TerrainTextureGen::defaultSplat({ 0.0f, 0.0f }, { 500.0f, 500.0f }, res, flat, sp);
    long rockRamp = 0, rockFlat = 0;
    for (size_t i = 1; i < splat.size(); i += 4) { rockRamp += splat[i]; rockFlat += splatFlat[i]; }
    CHECK(rockRamp > rockFlat);
    // Flat mid-height ground is dominated by grass (channel 0).
    long grassFlat = 0;
    for (size_t i = 0; i < splatFlat.size(); i += 4) grassFlat += splatFlat[i];
    CHECK(grassFlat > rockFlat);
}
