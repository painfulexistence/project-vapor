#version 450
// Adaptive GPU tessellation — instanced draw path, fragment stage (Vulkan
// twin of tessFragmentMain in 3d_tess_render.metal). Non-terrain instances
// keep the LoD debug hash color; TESS_FLAG_TERRAIN instances run the SAME
// world-space detail-layer splat as RHIMain.frag's shadeTerrain (weights
// recomputed per fragment from height/slope + FBm breakup, 4 detail layers
// tiled in world space, tangent-space detail normals perturbing the
// heightfield surface normal) so tessellated terrain textures identically to
// the streamed-tile path. Simple lambert + ambient either way.

layout(location = 0) in vec3 vWorldNormal;
layout(location = 1) in vec3 vWorldPosition;
layout(location = 2) in vec2 vUv;
layout(location = 3) in float vHeight01;
layout(location = 4) flat in float vTerrainMix;
layout(location = 5) flat in uint vDepth;

layout(location = 0) out vec4 outColor;

// Detail-layer arrays (grass/rock/dirt/snow), world-space tiled — the same
// two 2D arrays RHIMain.frag samples at set2 b13/b14, bound to this pipeline
// by tessRenderPass. Default white array when no terrain is staged.
layout(set = 0, binding = 4) uniform sampler2DArray terrainDetailAlbedo;
layout(set = 0, binding = 5) uniform sampler2DArray terrainDetailNormal;

vec3 tessHashColor(uint x) {
    x = (x ^ 61u) ^ (x >> 16u);
    x *= 9u;
    x = x ^ (x >> 4u);
    x *= 0x27d4eb2du;
    x = x ^ (x >> 15u);
    return vec3(float(x & 255u), float((x >> 8u) & 255u), float((x >> 16u) & 255u)) / 255.0;
}

// ── Terrain splat (verbatim from RHIMain.frag) ──────────────────────────────
// Per-layer world-space frequency (repeats/metre): grass 4 m, rock ~21 m,
// dirt 8 m, snow ~13 m.
const vec4 kTerrainLayerFreq = vec4(0.25, 0.046875, 0.125, 0.078125);
const uint kTerrainSplatSeed = 7u;

uint tgHash2(int x, int y, uint seed) {
    uint h = uint(x) * 374761393u + uint(y) * 668265263u + seed * 2654435761u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float tgHash01(int x, int y, uint seed) { return float(tgHash2(x, y, seed) >> 8) * (1.0 / 16777216.0); }
float tgSmooth(float t) { return t * t * (3.0 - 2.0 * t); }

// world-space (non-periodic) value-noise FBm, matching WorldFBm() in the CPU gen
float tgWorldFBm(vec2 p, float wavelength, int octaves, uint seed) {
    float sum = 0.0, amp = 0.5, freq = 1.0 / wavelength;
    for (int k = 0; k < octaves; ++k) {
        float u = p.x * freq, v = p.y * freq;
        int xi = int(floor(u)), yi = int(floor(v));
        float fx = tgSmooth(u - float(xi)), fy = tgSmooth(v - float(yi));
        uint s = seed + uint(k) * 131u;
        float a = tgHash01(xi, yi, s),     b = tgHash01(xi + 1, yi, s);
        float c = tgHash01(xi, yi + 1, s), d = tgHash01(xi + 1, yi + 1, s);
        sum += amp * mix(mix(a, b, fx), mix(c, d, fx), fy);
        amp *= 0.5; freq *= 2.0;
    }
    return sum;
}

// {grass, rock, dirt, snow} weights (defaultSplat rules, in-shader).
vec4 terrainSplatWeights(float height01, float slope, vec2 worldXZ) {
    float b1 = tgWorldFBm(worldXZ, 180.0, 3, kTerrainSplatSeed);
    float b2 = tgWorldFBm(worldXZ, 45.0, 3, kTerrainSplatSeed + 101u);
    float rock = smoothstep(0.55, 1.05, slope + 0.25 * (b1 - 0.5));
    float snowline = 0.62 + 0.08 * (b2 - 0.5);
    float snow = smoothstep(snowline, snowline + 0.16, height01) * (1.0 - 0.85 * rock);
    float dirt = 0.55 * smoothstep(0.5, 0.75, b2) * smoothstep(0.18, 0.45, slope + 0.2 * (b1 - 0.5));
    dirt += 0.6 * smoothstep(0.10, 0.04, height01);
    dirt = clamp(dirt, 0.0, 1.0) * (1.0 - rock) * (1.0 - snow);
    float grass = max(1.0 - rock - snow - dirt, 0.0);
    vec4 w = vec4(grass, rock, dirt, snow);
    return w / max(w.x + w.y + w.z + w.w, 1e-4);
}

void main() {
    vec3 N = normalize(vWorldNormal);
    vec3 albedo;
    if (vTerrainMix > 0.5) {
        // Slope (rise/run) from the interpolated heightfield normal — the tess
        // geometry is already dense, so the vertex normal carries the fine
        // detail RHIMain.frag has to reconstruct per pixel.
        float slope = length(N.xz) / max(N.y, 1e-3);
        vec4 w = terrainSplatWeights(vHeight01, slope, vWorldPosition.xz);
        vec3 c = vec3(0.0);
        vec3 dn = vec3(0.0);
        for (int i = 0; i < 4; ++i) {
            vec2 uv = vWorldPosition.xz * kTerrainLayerFreq[i];
            c  += w[i] * pow(texture(terrainDetailAlbedo, vec3(uv, float(i))).rgb, vec3(2.2));
            dn += w[i] * (texture(terrainDetailNormal, vec3(uv, float(i))).xyz * 2.0 - 1.0);
        }
        albedo = c;
        // Perturb the surface normal by the tangent-space detail normal.
        dn = normalize(dn + vec3(0.0, 0.0, 1e-4));
        vec3 T = normalize(vec3(1.0, 0.0, 0.0) - N * N.x);
        vec3 B = cross(N, T);
        N = normalize(T * dn.x + B * dn.y + N * dn.z);
    } else {
        albedo = tessHashColor(vDepth * 2654435761u);
    }

    vec3 lightDir = normalize(vec3(0.4, 1.0, 0.3));
    float ndl = max(dot(N, lightDir), 0.0);
    vec3 color = albedo * (0.25 + 0.75 * ndl);
    outColor = vec4(color, 1.0);
}
