#pragma once
// ============================================================================
// Vapor::proctex — procedural texture toolkit (engine-agnostic: glm + std).
//
// Bake-time RGBA8 image generators: tileable value-noise fBm, glazed ceramic
// tile faces (albedo / normal / roughness), noise-driven flat surfaces
// (plaster, grout, stone) and brushed metal. Grown out of Examples/Poolrooms
// and promoted engine-side so scenes, tools and the scene-JSON `textures`
// block can share one vocabulary.
//
// Conventions:
//  * Every generator returns a square, TILEABLE RGBA8 image; the noise lattice
//    wraps, so adjacent copies meet seamlessly.
//  * Tile generators author ONE tile face into the UV cell [0,1]^2 — the panel
//    geometry repeats it (see Vapor::procgen::buildTilePanel). Don't paint
//    grout lines: those are real geometry.
//  * Normal maps are tangent-space, +Z out (OpenGL / MikkTSpace convention).
//  * Roughness is written to RGB as grayscale; the shader reads one channel.
//  * Generators are pure functions of their arguments and a uint32 seed — no
//    global RNG — so a given (generator, params) pair is reproducible, which
//    is what lets the scene-JSON layer hash them into a cache key.
// ============================================================================

#include <glm/glm.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Vapor {
namespace proctex {

// Square RGBA8 image, row-major, 4 bytes per texel.
struct TextureData {
    uint32_t size = 0;
    std::vector<uint8_t> pixels;  // size*size*4

    bool empty() const { return size == 0 || pixels.empty(); }
    size_t texelCount() const { return size_t(size) * size; }
};

// Tileable value noise (wrapped lattice + smooth interpolation + fBm).
inline float hash2(int x, int y, uint32_t seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xFFFFFFu) / float(0xFFFFFFu);
}

// `period` is the lattice wrap length in cells and MUST be >= 1: the wrap below
// is a modulo by it, so a caller passing 0 (easy to do from a data file) would
// divide by zero and take the process down with SIGFPE.
inline float fbm(float x, float y, int period, int octaves, uint32_t seed) {
    period = period < 1 ? 1 : period;
    float sum = 0.0f, amp = 0.5f, freq = 1.0f;
    for (int o = 0; o < octaves; ++o) {
        const int p = period * int(freq);
        const float fx = x * freq, fy = y * freq;
        int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
        float tx = fx - float(x0), ty = fy - float(y0);
        tx = tx * tx * (3.0f - 2.0f * tx);
        ty = ty * ty * (3.0f - 2.0f * ty);
        auto wrap = [p](int v) { return ((v % p) + p) % p; };
        const float c00 = hash2(wrap(x0), wrap(y0), seed + uint32_t(o) * 101u);
        const float c10 = hash2(wrap(x0 + 1), wrap(y0), seed + uint32_t(o) * 101u);
        const float c01 = hash2(wrap(x0), wrap(y0 + 1), seed + uint32_t(o) * 101u);
        const float c11 = hash2(wrap(x0 + 1), wrap(y0 + 1), seed + uint32_t(o) * 101u);
        sum += ((c00 * (1 - tx) + c10 * tx) * (1 - ty) + (c01 * (1 - tx) + c11 * tx) * ty) * amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum;
}

// Largest edge a generator will bake. These are CPU-side bake-time images that
// end up in a scene cook, so the ceiling exists to keep a mistyped `size` in a
// data file from requesting a multi-gigabyte allocation: 8192 is already 256 MB
// of RGBA and far beyond any tile face.
inline constexpr uint32_t kMaxTextureSize = 8192;

inline constexpr uint32_t clampTextureSize(uint32_t size) {
    return size > kMaxTextureSize ? kMaxTextureSize : (size < 1u ? 1u : size);
}

inline TextureData makeImage(uint32_t size) {
    TextureData img;
    img.size = clampTextureSize(size);
    img.pixels.assign(size_t(img.size) * img.size * 4, 255);
    return img;
}

inline void putPixel(TextureData& img, uint32_t x, uint32_t y, glm::vec3 c) {
    const size_t i = (size_t(y) * img.size + x) * 4;
    img.pixels[i + 0] = uint8_t(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
    img.pixels[i + 1] = uint8_t(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
    img.pixels[i + 2] = uint8_t(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
    img.pixels[i + 3] = 255;
}

// Distance to the closest cell edge in cell space (0 at edges, 0.5 center).
inline float edgeDistance(float u, float v) {
    return glm::min(glm::min(u, 1.0f - u), glm::min(v, 1.0f - v));
}

// ── Tile face albedo ── glazed ceramic: soft center gradient, darker rim,
// speckle, faint diagonal glaze streaks.
inline TextureData tileAlbedo(glm::vec3 base, glm::vec3 rimTint, uint32_t seed, uint32_t size = 256) {
    size = clampTextureSize(size);  // loops below index img.size
    TextureData img = makeImage(size);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            const float e = edgeDistance(u, v);
            // Rim darkening in the outer 8% of the face.
            const float rim = glm::smoothstep(0.0f, 0.08f, e);
            // Gentle center sheen.
            const float sheen = 1.0f + 0.05f * (1.0f - glm::length(glm::vec2(u, v) - glm::vec2(0.5f)) * 2.0f);
            // Glaze streaks + speckle.
            const float streak = fbm(u * 3.0f + v * 5.0f, v * 3.0f, 4, 3, seed) - 0.5f;
            const float speck = fbm(u * 24.0f, v * 24.0f, 24, 2, seed + 7u) - 0.5f;
            glm::vec3 c = base * sheen * (1.0f + 0.045f * streak + 0.03f * speck);
            c = glm::mix(rimTint * base, c, glm::mix(0.55f, 1.0f, rim));
            putPixel(img, x, y, c);
        }
    }
    return img;
}

// ── Tile face normal map ── mostly flat glaze with a soft pillow toward the
// rim and low-amplitude waviness. Tangent-space, +Z out.
inline TextureData tileNormal(float pillow, float waviness, uint32_t seed, uint32_t size = 256) {
    size = clampTextureSize(size);  // loops below index img.size
    TextureData img = makeImage(size);
    auto height = [&](float u, float v) {
        // Pillow: raised center falling toward the edges (last 10%).
        const float e = edgeDistance(u, v);
        const float h = glm::smoothstep(0.0f, 0.10f, e) * pillow;
        return h + fbm(u * 6.0f, v * 6.0f, 6, 3, seed) * waviness;
    };
    const float d = 1.0f / size;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            const float dhdx = (height(u + d, v) - height(u - d, v)) / (2.0f * d);
            const float dhdy = (height(u, v + d) - height(u, v - d)) / (2.0f * d);
            const glm::vec3 n = glm::normalize(glm::vec3(-dhdx, -dhdy, 1.0f));
            putPixel(img, x, y, n * 0.5f + 0.5f);
        }
    }
    return img;
}

// ── Tile roughness ── grayscale in RGB (the shader reads one channel):
// smooth glaze center, rougher rim, smudges.
inline TextureData tileRoughness(float centerR, float rimR, uint32_t seed, uint32_t size = 256) {
    size = clampTextureSize(size);  // loops below index img.size
    TextureData img = makeImage(size);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            const float e = edgeDistance(u, v);
            const float rim = 1.0f - glm::smoothstep(0.0f, 0.10f, e);
            const float smudge = (fbm(u * 5.0f, v * 5.0f, 5, 3, seed) - 0.5f) * 0.12f;
            const float r = glm::clamp(glm::mix(centerR, rimR, rim) + smudge, 0.02f, 1.0f);
            putPixel(img, x, y, glm::vec3(r));
        }
    }
    return img;
}

// ── Flat noise-driven surfaces (plaster, grout, stone) ──
inline TextureData noisyAlbedo(glm::vec3 base, float variation, int period, uint32_t seed,
                              uint32_t size = 256) {
    size = clampTextureSize(size);  // loops below index img.size
    TextureData img = makeImage(size);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = float(x) / size, v = float(y) / size;
            const float n = fbm(u * period, v * period, period, 4, seed) - 0.5f;
            putPixel(img, x, y, base * (1.0f + variation * n));
        }
    }
    return img;
}

inline TextureData noisyNormal(float strength, int period, uint32_t seed, uint32_t size = 256) {
    size = clampTextureSize(size);  // loops below index img.size
    TextureData img = makeImage(size);
    auto h = [&](float u, float v) { return fbm(u * period, v * period, period, 4, seed); };
    const float d = 1.0f / size;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = float(x) / size, v = float(y) / size;
            const glm::vec3 n = glm::normalize(glm::vec3(
                -(h(u + d, v) - h(u - d, v)) / (2.0f * d) * strength,
                -(h(u, v + d) - h(u, v - d)) / (2.0f * d) * strength, 1.0f));
            putPixel(img, x, y, n * 0.5f + 0.5f);
        }
    }
    return img;
}

// ── Brushed metal ── horizontal streaks in albedo + roughness.
inline TextureData brushedMetalAlbedo(glm::vec3 base, uint32_t seed, uint32_t size = 256) {
    size = clampTextureSize(size);  // loops below index img.size
    TextureData img = makeImage(size);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = float(x) / size, v = float(y) / size;
            const float streak = fbm(u * 2.0f, v * 48.0f, 48, 2, seed) - 0.5f;
            putPixel(img, x, y, base * (1.0f + 0.08f * streak));
        }
    }
    return img;
}

inline TextureData brushedMetalRoughness(float base, uint32_t seed, uint32_t size = 256) {
    size = clampTextureSize(size);  // loops below index img.size
    TextureData img = makeImage(size);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float u = float(x) / size, v = float(y) / size;
            const float streak = fbm(u * 2.0f, v * 48.0f, 48, 2, seed + 5u) - 0.5f;
            putPixel(img, x, y, glm::vec3(glm::clamp(base + 0.10f * streak, 0.05f, 1.0f)));
        }
    }
    return img;
}

}  // namespace proctex
}  // namespace Vapor
