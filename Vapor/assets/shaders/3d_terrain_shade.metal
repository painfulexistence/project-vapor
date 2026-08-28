// ── Terrain surface shading (shaderModel == 1) — MSL twin of RHIMain.frag's
// branch, shared by every Metal fragment that shades terrain (forward PBR in
// 3d_pbr_normal_mapped.metal, both its bound and bindless/ICB paths, and the
// meshlet fragment in 3d_meshlet.metal). Faithful port of Atmospheric's
// terrain.frag 4-layer splat: weights recomputed per fragment from
// height/slope + world-space FBm breakup (defaultSplat rules, no splat
// texture), detail layers tiled in world space. Layer order/frequency
// (repeats per metre): 0 grass 0.25, 1 rock 0.046875, 2 dirt 0.125,
// 3 snow 0.078125.
//
// Self-contained: needs only <metal_stdlib> and the FastNoiseLite twin it
// includes below. Include once per translation unit (both the runtime include
// expander and the build-time flattener dedupe repeats).
constant float4 kTerrainLayerFreq = float4(0.25, 0.046875, 0.125, 0.078125);
constant uint kTerrainSplatSeed = 7u;

inline uint trgHash2(int x, int y, uint seed) {
    uint h = uint(x) * 374761393u + uint(y) * 668265263u + seed * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
inline float trgHash01(int x, int y, uint seed) { return float(trgHash2(x, y, seed) >> 8) * (1.0 / 16777216.0); }
inline float trgSmooth(float t) { return t * t * (3.0 - 2.0 * t); }

static float trgWorldFBm(float2 p, float wavelength, int octaves, uint seed) {
    float sum = 0.0, amp = 0.5, freq = 1.0 / wavelength;
    for (int k = 0; k < octaves; ++k) {
        float u = p.x * freq, v = p.y * freq;
        int xi = int(floor(u)), yi = int(floor(v));
        float fx = trgSmooth(u - float(xi)), fy = trgSmooth(v - float(yi));
        uint sd = seed + uint(k) * 131u;
        float a = trgHash01(xi, yi, sd),     b = trgHash01(xi + 1, yi, sd);
        float c = trgHash01(xi, yi + 1, sd), d = trgHash01(xi + 1, yi + 1, sd);
        sum += amp * mix(mix(a, b, fx), mix(c, d, fx), fy);
        amp *= 0.5; freq *= 2.0;
    }
    return sum;
}

static float4 trgSplatWeights(float height01, float slope, float2 worldXZ) {
    float b1 = trgWorldFBm(worldXZ, 180.0, 3, kTerrainSplatSeed);
    float b2 = trgWorldFBm(worldXZ, 45.0, 3, kTerrainSplatSeed + 101u);
    float rock = smoothstep(0.55, 1.05, slope + 0.25 * (b1 - 0.5));
    float snowline = 0.62 + 0.08 * (b2 - 0.5);
    float snow = smoothstep(snowline, snowline + 0.16, height01) * (1.0 - 0.85 * rock);
    float dirt = 0.55 * smoothstep(0.5, 0.75, b2) * smoothstep(0.18, 0.45, slope + 0.2 * (b1 - 0.5));
    dirt += 0.6 * smoothstep(0.10, 0.04, height01);
    dirt = clamp(dirt, 0.0, 1.0) * (1.0 - rock) * (1.0 - snow);
    float grass = max(1.0 - rock - snow - dirt, 0.0);
    float4 w = float4(grass, rock, dirt, snow);
    return w / max(w.x + w.y + w.z + w.w, 1e-4);
}

// Terrain height field — the shared MSL FastNoiseLite twin (see the include):
// trhHeightAt evaluates the SAME OpenSimplex2 FBm field TerrainWorld::heightAt
// builds the streamed mesh on, letting the fragment stage reconstruct a
// per-pixel surface normal that restores the octaves the coarse LOD mesh
// vertices smooth away. Params arrive packed in the terrain material's unused
// Disney lobe fields.
#include "Res/shaders/3d_terrain_noise.metal"

static void trgShadeTerrain(float3 worldPos, float noiseFreq, int octaves, uint seed, float heightScale,
                            float height01,
                            texture2d_array<float, access::sample> detailAlbedo,
                            texture2d_array<float, access::sample> detailNormal,
                            thread float3& outAlbedo, thread float3& outN) {
    constexpr sampler ts(address::repeat, filter::linear, mip_filter::linear);
    // Central-difference normal at the pixel's world-space footprint (>= 1 m),
    // so distant terrain band-limits the noise (no shimmer) while near terrain
    // resolves the finest octave. Sign matches buildTileGeometry's vertex normal.
    float fp = max(max(abs(dfdx(worldPos.x)), abs(dfdy(worldPos.x))),
                   max(abs(dfdx(worldPos.z)), abs(dfdy(worldPos.z))));
    float d = clamp(fp, 1.0, 64.0);
    // Band-limit the height field to the pixel's footprint: an octave whose
    // wavelength is under 4d aliases in a central difference at spacing d
    // (the two taps are 2d apart, so 4d is the Nyquist wavelength) — it
    // contributes shimmer, not detail, and it is the terrain fragment's main
    // cost (4 taps x N simplex evals). Distant pixels, which dominate the
    // screen, fall to 3-5 octaves instead of 9. CONTINUOUS count: trhHeightAt
    // fades the final octave by the fractional part, and normalizes by the
    // full octave count, so the surface has no visible LOD rings. All four
    // taps share it, keeping the reconstructed normal consistent.
    // (GLSL twin: RHIMain.frag shadeTerrain.)
    float lodOct = clamp(log2(1.0 / (noiseFreq * 4.0 * d)) + 1.0, 3.0, float(octaves));
    float hl = trhHeightAt(worldPos.xz - float2(d, 0.0), noiseFreq, octaves, lodOct, seed, heightScale);
    float hr = trhHeightAt(worldPos.xz + float2(d, 0.0), noiseFreq, octaves, lodOct, seed, heightScale);
    float hb = trhHeightAt(worldPos.xz - float2(0.0, d), noiseFreq, octaves, lodOct, seed, heightScale);
    float ht = trhHeightAt(worldPos.xz + float2(0.0, d), noiseFreq, octaves, lodOct, seed, heightScale);
    float3 baseN = normalize(float3(hl - hr, 2.0 * d, hb - ht));

    float slope = length(baseN.xz) / max(baseN.y, 1e-3);  // rise/run
    float4 w = trgSplatWeights(height01, slope, worldPos.xz);
    float2 wp = worldPos.xz;
    float3 c = float3(0.0);
    float3 dn = float3(0.0);
    for (int i = 0; i < 4; ++i) {
        float2 uv = wp * kTerrainLayerFreq[i];
        c  += w[i] * pow(detailAlbedo.sample(ts, uv, i).rgb, float3(2.2));
        dn += w[i] * (detailNormal.sample(ts, uv, i).xyz * 2.0 - 1.0);
    }
    outAlbedo = c;
    dn = normalize(dn + float3(0.0, 0.0, 1e-4));
    float3 nn = baseN;
    float3 T = normalize(float3(1.0, 0.0, 0.0) - nn * nn.x);
    float3 B = cross(nn, T);
    outN = normalize(T * dn.x + B * dn.y + nn * dn.z);
}
