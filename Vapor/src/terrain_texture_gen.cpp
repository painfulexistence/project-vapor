#include "terrain_texture_gen.hpp"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

// All synthesis is plain CPU math over integer lattices, so results are
// deterministic per (res, seed) on every platform. Detail textures use
// PERIODIC value noise (lattice coordinates wrap at the octave period) which
// is what makes them tile seamlessly; the splat generator uses world-space
// (non-periodic) noise because tiles must agree across borders in world
// coordinates instead. Ported verbatim from Atmospheric's terrain_texture_gen.

namespace Vapor {
namespace {

inline Uint32 ttgHash2(int x, int y, Uint32 seed) {
    Uint32 h = static_cast<Uint32>(x) * 374761393u + static_cast<Uint32>(y) * 668265263u + seed * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
inline float ttgHash01(int x, int y, Uint32 seed) {
    return static_cast<float>(ttgHash2(x, y, seed) >> 8) * (1.0f / 16777216.0f);
}

inline float ttgSmooth(float t) { return t * t * (3.0f - 2.0f * t); }

// Value noise on a lattice that wraps every `period` cells — the basis of
// every tileable map below. u,v in [0, period).
float periodicNoise(float u, float v, int period, Uint32 seed) {
    const int xi = static_cast<int>(std::floor(u)), yi = static_cast<int>(std::floor(v));
    const float fx = ttgSmooth(u - xi), fy = ttgSmooth(v - yi);
    const int x0 = ((xi % period) + period) % period, y0 = ((yi % period) + period) % period;
    const int x1 = (x0 + 1) % period, y1 = (y0 + 1) % period;
    const float a = ttgHash01(x0, y0, seed), b = ttgHash01(x1, y0, seed);
    const float c = ttgHash01(x0, y1, seed), d = ttgHash01(x1, y1, seed);
    return glm::mix(glm::mix(a, b, fx), glm::mix(c, d, fx), fy);
}

// Tileable FBm: octave k doubles both frequency and lattice period, so every
// octave wraps at the texture edge. uv in [0,1).
float tileFBm(glm::vec2 uv, int basePeriod, int octaves, Uint32 seed, float gain = 0.5f) {
    float sum = 0.0f, amp = 0.5f;
    int period = basePeriod;
    for (int k = 0; k < octaves; ++k) {
        sum += amp * periodicNoise(uv.x * period, uv.y * period, period, seed + k * 131u);
        amp *= gain;
        period *= 2;
    }
    return sum;
}

// Ridged variant: sharp crests where the noise crosses its midline — reads as
// rock strata / erosion channels.
float tileRidged(glm::vec2 uv, int basePeriod, int octaves, Uint32 seed) {
    float sum = 0.0f, amp = 0.5f;
    int period = basePeriod;
    for (int k = 0; k < octaves; ++k) {
        const float n = periodicNoise(uv.x * period, uv.y * period, period, seed + k * 131u);
        const float r = 1.0f - std::abs(2.0f * n - 1.0f);
        sum += amp * r * r;
        amp *= 0.55f;
        period *= 2;
    }
    return sum;
}

// World-space (non-periodic) FBm for splat breakup. wavelength in metres.
float worldFBm(glm::vec2 p, float wavelength, int octaves, Uint32 seed) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f / wavelength;
    for (int k = 0; k < octaves; ++k) {
        const float u = p.x * freq, v = p.y * freq;
        const int xi = static_cast<int>(std::floor(u)), yi = static_cast<int>(std::floor(v));
        const float fx = ttgSmooth(u - xi), fy = ttgSmooth(v - yi);
        const float a = ttgHash01(xi, yi, seed + k * 131u), b = ttgHash01(xi + 1, yi, seed + k * 131u);
        const float c = ttgHash01(xi, yi + 1, seed + k * 131u), d = ttgHash01(xi + 1, yi + 1, seed + k * 131u);
        sum += amp * glm::mix(glm::mix(a, b, fx), glm::mix(c, d, fx), fy);
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum;
}

inline Uint8 toByte(float v) {
    return static_cast<Uint8>(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Tangent-space normal map from a wrapped heightfield (+Z out, matching the
// terrain shader's layer-normal decode).
std::vector<Uint8> normalFromHeight(const std::vector<float>& h, int res, float strength) {
    std::vector<Uint8> out(static_cast<size_t>(res) * res * 4);
    for (int y = 0; y < res; ++y) {
        for (int x = 0; x < res; ++x) {
            const float hl = h[static_cast<size_t>(y) * res + (x + res - 1) % res];
            const float hr = h[static_cast<size_t>(y) * res + (x + 1) % res];
            const float hd = h[static_cast<size_t>((y + res - 1) % res) * res + x];
            const float hu = h[static_cast<size_t>((y + 1) % res) * res + x];
            const glm::vec3 n = glm::normalize(glm::vec3((hl - hr) * strength, (hd - hu) * strength, 1.0f));
            Uint8* px = &out[(static_cast<size_t>(y) * res + x) * 4];
            px[0] = toByte(n.x * 0.5f + 0.5f);
            px[1] = toByte(n.y * 0.5f + 0.5f);
            px[2] = toByte(n.z * 0.5f + 0.5f);
            px[3] = 255;
        }
    }
    return out;
}

std::shared_ptr<Image> makeImage(const char* uri, const std::vector<Uint8>& rgba, int res) {
    auto img = std::make_shared<Image>();
    img->uri = uri;  // unique key for the renderer's texture cache
    img->width = static_cast<Uint32>(res);
    img->height = static_cast<Uint32>(res);
    img->channelCount = 4;
    img->byteArray = rgba;
    return img;
}

TerrainTextureGen::DetailLayer makeLayer(const char* albedoUri, const char* normalUri,
                                         const std::vector<Uint8>& albedo, const std::vector<float>& height,
                                         int res, float normalStrength) {
    TerrainTextureGen::DetailLayer layer;
    layer.albedo = makeImage(albedoUri, albedo, res);
    layer.normal = makeImage(normalUri, normalFromHeight(height, res, normalStrength), res);
    return layer;
}

}// namespace

namespace TerrainTextureGen {

// ── Grass / meadow ───────────────────────────────────────────────────────
// Moisture drifts (low-freq) shade rich green vs dry olive; blade clumps
// (high-freq) add the fine structure; sparse dry-straw patches break the
// repeat. Albedo is authored in display space (the shader gamma-decodes).
DetailLayer generateGrass(int res, Uint32 seed) {
    std::vector<Uint8> albedo(static_cast<size_t>(res) * res * 4);
    std::vector<float> height(static_cast<size_t>(res) * res);
    const glm::vec3 richGreen(0.26f, 0.36f, 0.13f), oliveGreen(0.38f, 0.40f, 0.18f);
    const glm::vec3 soil(0.22f, 0.18f, 0.10f), straw(0.60f, 0.52f, 0.28f);
    for (int y = 0; y < res; ++y) {
        for (int x = 0; x < res; ++x) {
            const glm::vec2 uv(static_cast<float>(x) / res, static_cast<float>(y) / res);
            const float moisture = tileFBm(uv, 4, 4, seed);
            const float clump = tileFBm(uv, 48, 4, seed + 7);
            const float dry = glm::smoothstep(0.62f, 0.78f, tileFBm(uv, 6, 3, seed + 13));
            glm::vec3 c = glm::mix(oliveGreen, richGreen, glm::smoothstep(0.3f, 0.7f, moisture));
            // Soft soil show-through between clumps — a wide smoothstep, or the
            // soil reads as harsh black speckle at distance.
            c = glm::mix(soil, c, 0.35f + 0.65f * glm::smoothstep(0.2f, 0.5f, clump));
            c = glm::mix(c, straw, dry * 0.65f);
            c *= 0.9f + 0.2f * ttgHash01(x, y, seed + 29);  // per-texel blade sparkle
            Uint8* px = &albedo[(static_cast<size_t>(y) * res + x) * 4];
            px[0] = toByte(c.r);
            px[1] = toByte(c.g);
            px[2] = toByte(c.b);
            px[3] = 255;
            height[static_cast<size_t>(y) * res + x] = 0.75f * clump + 0.25f * moisture;
        }
    }
    return makeLayer("terraintexgen_grass_albedo", "terraintexgen_grass_normal", albedo, height, res, 2.5f);
}

// ── Rock / cliff ─────────────────────────────────────────────────────────
// Ridged FBm reads as strata + erosion channels; crevices are darkened by the
// same height (cheap baked AO) and a warm mineral tint varies slabs.
DetailLayer generateRock(int res, Uint32 seed) {
    std::vector<Uint8> albedo(static_cast<size_t>(res) * res * 4);
    std::vector<float> height(static_cast<size_t>(res) * res);
    const glm::vec3 darkRock(0.21f, 0.19f, 0.18f), lightRock(0.52f, 0.48f, 0.44f);
    const glm::vec3 ironTint(0.48f, 0.38f, 0.30f);
    for (int y = 0; y < res; ++y) {
        for (int x = 0; x < res; ++x) {
            const glm::vec2 uv(static_cast<float>(x) / res, static_cast<float>(y) / res);
            const float ridge = tileRidged(uv, 5, 5, seed);
            const float grain = tileFBm(uv, 64, 3, seed + 7);
            const float slab = tileFBm(uv, 3, 2, seed + 13);
            const float h = std::clamp(0.85f * ridge + 0.15f * grain, 0.0f, 1.0f);
            glm::vec3 c = glm::mix(darkRock, lightRock, h);
            c = glm::mix(c, ironTint, glm::smoothstep(0.55f, 0.8f, slab) * 0.35f);
            c *= 0.55f + 0.45f * h;  // crevice AO
            Uint8* px = &albedo[(static_cast<size_t>(y) * res + x) * 4];
            px[0] = toByte(c.r);
            px[1] = toByte(c.g);
            px[2] = toByte(c.b);
            px[3] = 255;
            height[static_cast<size_t>(y) * res + x] = h;
        }
    }
    return makeLayer("terraintexgen_rock_albedo", "terraintexgen_rock_normal", albedo, height, res, 6.0f);
}

// ── Dirt / scree ─────────────────────────────────────────────────────────
// Mid-freq soil undulation with embedded pebbles (bright speckle where the
// high-freq noise spikes) — the worn transitional ground between grass and
// rock faces.
DetailLayer generateDirt(int res, Uint32 seed) {
    std::vector<Uint8> albedo(static_cast<size_t>(res) * res * 4);
    std::vector<float> height(static_cast<size_t>(res) * res);
    const glm::vec3 darkSoil(0.27f, 0.21f, 0.15f), lightSoil(0.47f, 0.38f, 0.27f);
    const glm::vec3 pebble(0.55f, 0.52f, 0.47f);
    for (int y = 0; y < res; ++y) {
        for (int x = 0; x < res; ++x) {
            const glm::vec2 uv(static_cast<float>(x) / res, static_cast<float>(y) / res);
            const float soilN = tileFBm(uv, 8, 4, seed);
            // 2-octave FBm sums to [0, 0.75] around a 0.375 mean — the pebble
            // threshold must sit inside that range or none appear.
            const float spike = tileFBm(uv, 96, 2, seed + 7);
            const float stone = glm::smoothstep(0.50f, 0.60f, spike);
            glm::vec3 c = glm::mix(darkSoil, lightSoil, soilN);
            c = glm::mix(c, pebble, stone);
            c *= 0.92f + 0.16f * ttgHash01(x, y, seed + 29);
            Uint8* px = &albedo[(static_cast<size_t>(y) * res + x) * 4];
            px[0] = toByte(c.r);
            px[1] = toByte(c.g);
            px[2] = toByte(c.b);
            px[3] = 255;
            height[static_cast<size_t>(y) * res + x] = 0.6f * soilN + 0.4f * stone;
        }
    }
    return makeLayer("terraintexgen_dirt_albedo", "terraintexgen_dirt_normal", albedo, height, res, 5.0f);
}

// ── Snow ─────────────────────────────────────────────────────────────────
// Wind-packed drifts (smooth low-freq) with a blue-shadowed trough tint and
// rare glint texels; deliberately low normal strength so peaks read soft
// against the hard rock layer.
DetailLayer generateSnow(int res, Uint32 seed) {
    std::vector<Uint8> albedo(static_cast<size_t>(res) * res * 4);
    std::vector<float> height(static_cast<size_t>(res) * res);
    const glm::vec3 snowLit(0.93f, 0.95f, 0.98f), snowShade(0.72f, 0.78f, 0.88f);
    for (int y = 0; y < res; ++y) {
        for (int x = 0; x < res; ++x) {
            const glm::vec2 uv(static_cast<float>(x) / res, static_cast<float>(y) / res);
            const float drift = tileFBm(uv, 3, 4, seed);
            const float glint = ttgHash01(x, y, seed + 29) > 0.995f ? 0.15f : 0.0f;
            glm::vec3 c = glm::mix(snowShade, snowLit, glm::smoothstep(0.25f, 0.75f, drift)) + glint;
            Uint8* px = &albedo[(static_cast<size_t>(y) * res + x) * 4];
            px[0] = toByte(c.r);
            px[1] = toByte(c.g);
            px[2] = toByte(c.b);
            px[3] = 255;
            height[static_cast<size_t>(y) * res + x] = drift;
        }
    }
    return makeLayer("terraintexgen_snow_albedo", "terraintexgen_snow_normal", albedo, height, res, 1.5f);
}

// ── Splat weights ────────────────────────────────────────────────────────
std::vector<Uint8> defaultSplat(glm::vec2 worldMin, glm::vec2 worldMax, int res,
                                const std::function<float(float, float)>& height01, const SplatParams& p) {
    std::vector<Uint8> out(static_cast<size_t>(res) * res * 4);
    const glm::vec2 span = worldMax - worldMin;
    const float d = 2.0f;  // slope sample offset in metres

    for (int j = 0; j < res; ++j) {
        const float wz = worldMin.y + (j + 0.5f) / res * span.y;
        for (int i = 0; i < res; ++i) {
            const float wx = worldMin.x + (i + 0.5f) / res * span.x;

            const float h = height01(wx, wz);
            const float dhx = (height01(wx + d, wz) - height01(wx - d, wz)) * p.heightScale / (2.0f * d);
            const float dhz = (height01(wx, wz + d) - height01(wx, wz - d)) * p.heightScale / (2.0f * d);
            const float slope = std::sqrt(dhx * dhx + dhz * dhz);  // rise/run

            // Large-scale breakup so material borders wander instead of tracing
            // iso-lines, plus a finer grain for patch edges.
            const float b1 = worldFBm(glm::vec2(wx, wz), 180.0f, 3, p.seed);
            const float b2 = worldFBm(glm::vec2(wx, wz), 45.0f, 3, p.seed + 101);

            // Rock owns steep faces; the noisy threshold breaks the band.
            const float rock = glm::smoothstep(p.rockSlopeStart, p.rockSlopeFull, slope + 0.25f * (b1 - 0.5f));

            // Snow above a wandering snowline, pushed off sheer cliffs.
            const float snowline = p.snowStart + 0.08f * (b2 - 0.5f);
            const float snow =
                glm::smoothstep(snowline, snowline + (p.snowFull - p.snowStart), h) * (1.0f - 0.85f * rock);

            // Dirt/scree: worn patches on moderate slopes + valley-floor
            // sediment, never on top of rock or snow.
            float dirt = p.dirtAmount * glm::smoothstep(0.5f, 0.75f, b2)
                * glm::smoothstep(0.18f, 0.45f, slope + 0.2f * (b1 - 0.5f));
            dirt += 0.6f * glm::smoothstep(0.10f, 0.04f, h);
            dirt = std::clamp(dirt, 0.0f, 1.0f) * (1.0f - rock) * (1.0f - snow);

            const float grass = std::max(1.0f - rock - snow - dirt, 0.0f);
            const float sum = std::max(grass + rock + dirt + snow, 1e-4f);

            Uint8* px = &out[(static_cast<size_t>(j) * res + i) * 4];
            px[0] = toByte(grass / sum);
            px[1] = toByte(rock / sum);
            px[2] = toByte(dirt / sum);
            px[3] = toByte(snow / sum);
        }
    }
    return out;
}

}// namespace TerrainTextureGen

}// namespace Vapor
