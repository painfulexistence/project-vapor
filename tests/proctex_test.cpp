#include <catch2/catch_test_macros.hpp>
#include "Vapor/proctex.hpp"
#include <cmath>

// ============================================================================
// Vapor::proctex verification. The properties here are the ones the scene
// layer and the renderer actually depend on:
//   - determinism, because the scene-JSON layer hashes the baked pixels into
//     a texture-cache key (a generator that wandered between runs would break
//     both the cache and the scene cook)
//   - tiling, for the SURFACE generators only — tile FACE generators author a
//     bounded tile and their borders are real edges
//   - well-formed RGBA8 output and valid tangent-space normals
//   - survival of hostile arguments, since these are reachable from data
// ============================================================================

using namespace Vapor;
using namespace Vapor::proctex;

namespace {

// Mean per-channel step across the wrap seam, and across a typical interior
// column, for comparison. A tiling image shows no step the interior doesn't.
struct Seam {
    double wrapX = 0.0, interiorX = 0.0, wrapY = 0.0, interiorY = 0.0;
};

Seam measureSeam(const TextureData& t) {
    const uint32_t n = t.size;
    auto px = [&](uint32_t x, uint32_t y, int c) {
        return double(t.pixels[(size_t(y) * n + x) * 4 + c]);
    };
    Seam s;
    for (uint32_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            s.wrapX += std::abs(px(0, i, c) - px(n - 1, i, c));
            s.interiorX += std::abs(px(n / 2, i, c) - px(n / 2 - 1, i, c));
            s.wrapY += std::abs(px(i, 0, c) - px(i, n - 1, c));
            s.interiorY += std::abs(px(i, n / 2, c) - px(i, n / 2 - 1, c));
        }
    }
    const double k = 3.0 * n;
    return { s.wrapX / k, s.interiorX / k, s.wrapY / k, s.interiorY / k };
}

void checkWellFormed(const TextureData& t, uint32_t expectSize) {
    CHECK(t.size == expectSize);
    CHECK(t.pixels.size() == size_t(expectSize) * expectSize * 4);
    CHECK(t.texelCount() == size_t(expectSize) * expectSize);
    CHECK_FALSE(t.empty());
    size_t badAlpha = 0;
    for (size_t i = 3; i < t.pixels.size(); i += 4)
        if (t.pixels[i] != 255) ++badAlpha;
    CHECK(badAlpha == 0);
}

}  // namespace

TEST_CASE("every generator bakes a well-formed opaque RGBA8 image", "[proctex]") {
    checkWellFormed(tileAlbedo(glm::vec3(0.8f), glm::vec3(0.85f), 1u), 256);
    checkWellFormed(tileNormal(0.3f, 0.1f, 2u), 256);
    checkWellFormed(tileRoughness(0.1f, 0.3f, 3u), 256);
    checkWellFormed(noisyAlbedo(glm::vec3(0.7f), 0.1f, 8, 4u), 256);
    checkWellFormed(noisyNormal(0.3f, 12, 5u), 256);
    checkWellFormed(brushedMetalAlbedo(glm::vec3(0.75f), 6u), 256);
    checkWellFormed(brushedMetalRoughness(0.3f, 7u), 256);
    // A non-default edge is honoured.
    checkWellFormed(tileAlbedo(glm::vec3(0.8f), glm::vec3(0.85f), 8u, 64u), 64);
    checkWellFormed(noisyAlbedo(glm::vec3(0.7f), 0.1f, 8, 9u, 128u), 128);
}

TEST_CASE("generators are deterministic functions of their arguments", "[proctex]") {
    // The scene layer content-addresses these bakes, so identical arguments
    // must give identical bytes and a changed seed must not.
    CHECK(noisyAlbedo(glm::vec3(0.7f), 0.2f, 8, 31u).pixels ==
          noisyAlbedo(glm::vec3(0.7f), 0.2f, 8, 31u).pixels);
    CHECK(noisyAlbedo(glm::vec3(0.7f), 0.2f, 8, 31u).pixels !=
          noisyAlbedo(glm::vec3(0.7f), 0.2f, 8, 32u).pixels);
    CHECK(tileAlbedo(glm::vec3(0.8f), glm::vec3(0.85f), 5u).pixels ==
          tileAlbedo(glm::vec3(0.8f), glm::vec3(0.85f), 5u).pixels);
    // The whole uint32 seed range is usable.
    CHECK(noisyAlbedo(glm::vec3(0.7f), 0.2f, 8, 3000000000u).pixels !=
          noisyAlbedo(glm::vec3(0.7f), 0.2f, 8, 4000000000u).pixels);
}

TEST_CASE("surface generators tile seamlessly on both axes", "[proctex]") {
    // These repeat across a wall or along a rail, so the wrap seam must be no
    // worse than an ordinary interior step.
    for (const TextureData& t : { noisyAlbedo(glm::vec3(0.7f), 0.3f, 8, 21u),
                                  noisyNormal(0.5f, 12, 22u),
                                  brushedMetalAlbedo(glm::vec3(0.75f), 23u),
                                  brushedMetalRoughness(0.28f, 24u) }) {
        const Seam s = measureSeam(t);
        CHECK(s.wrapX <= s.interiorX * 3.0 + 1.0);
        CHECK(s.wrapY <= s.interiorY * 3.0 + 1.0);
    }
}

TEST_CASE("brushed metal wraps across x despite its anisotropic lattice", "[proctex]") {
    // Its streaks sample x over [0,2) and y over [0,48), so the two axes need
    // different lattice periods; using one period for both left a vertical
    // seam down the rails.
    const Seam s = measureSeam(brushedMetalAlbedo(glm::vec3(0.75f), 101u));
    CHECK(s.wrapX <= s.interiorX + 0.5);
}

TEST_CASE("fbm accepts an anisotropic lattice and stays finite", "[proctex]") {
    // Equal periods must agree with the square-lattice overload.
    CHECK(fbm(0.3f, 0.7f, 8, 8, 3, 1u) == fbm(0.3f, 0.7f, 8, 3, 1u));
    // The two periods only diverge once x passes the smaller one — below it
    // both wraps are the identity, so sample beyond x = 2.
    CHECK(fbm(3.3f, 0.7f, 2, 48, 3, 1u) != fbm(3.3f, 0.7f, 48, 48, 3, 1u));
    CHECK(fbm(0.3f, 0.7f, 2, 48, 3, 1u) == fbm(0.3f, 0.7f, 48, 48, 3, 1u));
    // Wrapping is exact at the period boundary on each axis independently.
    CHECK(fbm(0.0f, 0.25f, 2, 48, 2, 7u) == fbm(2.0f, 0.25f, 2, 48, 2, 7u));
    CHECK(fbm(0.25f, 0.0f, 2, 48, 2, 7u) == fbm(0.25f, 48.0f, 2, 48, 2, 7u));
}

TEST_CASE("normal maps encode unit-length tangent-space vectors, +Z out", "[proctex]") {
    for (const TextureData& t : { tileNormal(0.35f, 0.10f, 41u), noisyNormal(0.4f, 10, 42u) }) {
        size_t badZ = 0, badLen = 0;
        for (size_t i = 0; i < t.pixels.size(); i += 4) {
            const glm::vec3 v =
                glm::vec3(t.pixels[i], t.pixels[i + 1], t.pixels[i + 2]) / 127.5f - 1.0f;
            if (v.z <= 0.0f) ++badZ;
            if (std::abs(glm::length(v) - 1.0f) > 0.06f) ++badLen;
        }
        CHECK(badZ == 0);
        CHECK(badLen == 0);
    }
}

TEST_CASE("hostile arguments are clamped instead of crashing or exhausting memory", "[proctex]") {
    // period reaches a modulo inside the lattice; 0 used to be a SIGFPE.
    CHECK(std::isfinite(fbm(0.3f, 0.7f, 0, 4, 1u)));
    CHECK(std::isfinite(fbm(0.3f, 0.7f, 0, 0, 4, 1u)));
    CHECK(noisyAlbedo(glm::vec3(0.7f), 0.1f, 0, 1u, 32u).pixels.size() == 32u * 32u * 4u);
    CHECK(noisyNormal(0.3f, -8, 1u, 32u).pixels.size() == 32u * 32u * 4u);

    // An oversized edge would ask for a multi-gigabyte allocation. The clamp
    // must also keep the buffer consistent with the loop bound: generators
    // index img.size, so a clamped buffer with an unclamped loop would run off
    // the end.
    const TextureData huge = tileAlbedo(glm::vec3(0.8f), glm::vec3(0.85f), 1u, 100000u);
    CHECK(huge.size == kMaxTextureSize);
    CHECK(huge.pixels.size() == size_t(huge.size) * huge.size * 4);

    const TextureData zero = noisyAlbedo(glm::vec3(0.7f), 0.1f, 8, 1u, 0u);
    CHECK(zero.size == 1);
    CHECK(zero.pixels.size() == 4);

    CHECK(clampTextureSize(0) == 1);
    CHECK(clampTextureSize(256) == 256);
    CHECK(clampTextureSize(kMaxTextureSize + 1) == kMaxTextureSize);
}
