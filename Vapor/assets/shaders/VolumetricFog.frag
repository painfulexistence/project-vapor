#version 450
// True volumetric raymarched fog. GLSL twin of the Metal simpleFogFragment
// (3d_volumetric_fog.metal): 32 steps along the view ray, per-step
// in-scattering from the FULL light set — sun with a PSSM cascade tap per step
// (volumetric light shafts), the pixel's tile-culled point-light list, spot
// cones, and rect area lights (center-point x area approximation) — with
// Henyey-Greenstein phase and Beer-Lambert transmittance.

layout(location = 0) in vec2 tex_uv;
layout(location = 0) out vec4 outColor;

layout(set = 2, binding = 0) uniform sampler2D sceneColor;
layout(set = 2, binding = 1) uniform sampler2D sceneDepth;
layout(set = 2, binding = 2) uniform sampler2DArray pssmShadowMaps;

// Must match Vapor::FogRenderData (std430).
layout(std430, set = 1, binding = 0) readonly buffer FogBuf {
    mat4 invViewProj;
    vec3 cameraPosition; float _p0;
    vec3 sunDirection;   float _p1;
    vec3 sunColor;       float sunIntensity;
    float fogDensity;
    float fogHeightFalloff;
    float anisotropy;
    float ambientIntensity;
    float fogBaseHeight;
    float fogMaxHeight;
    float noiseScale;
    float noiseIntensity;
    float windSpeed;
    float time;
    vec2 _pad2;
    vec3 windDirection;
    uint volumeCount;   // fog volumes in FogVolumeBuf (0 = legacy single-volume height fog)
};

// Must match Vapor::PSSMRenderData (same layout RHIMain.frag reads).
layout(std430, set = 1, binding = 1) readonly buffer PSSMBuf {
    mat4 shadowLightSpaceMatrices[3];
    vec4 cascadeSplits;
    float shadowBlendRange;
};

const uint MAX_LIGHTS_PER_TILE = 256;
struct Cluster {
    vec4 mn;
    vec4 mx;
    uint lightCount;
    uint lightIndices[MAX_LIGHTS_PER_TILE];
};
layout(std430, set = 1, binding = 2) readonly buffer ClusterBuf { Cluster tiles[]; };

// Must match Vapor::PointLightData (stride 48).
struct PointLight {
    vec3 position;
    float _pad1;
    vec3 color;
    float _pad2;
    float intensity;
    float radius;
    vec2 _pad3;
};
layout(std430, set = 1, binding = 3) readonly buffer PointLightBuf { PointLight pointLights[]; };

// Must match Vapor::SpotLight (scalars follow the vectors).
struct SpotLight {
    vec3 position;
    float _pad0;
    vec3 direction;   // normalized, FROM the light
    float _pad1;
    vec3 color;
    float _pad2;
    float radius;
    float cosInner;
    float cosOuter;
    float intensity;
};
layout(std430, set = 1, binding = 4) readonly buffer SpotLightBuf { SpotLight spotLights[]; };

// Must match Vapor::RectLight (std430 legally packs the scalars into the vec3
// tails, unlike MSL — same 64-byte layout as the C++ struct).
struct RectLight {
    vec3 position;  float halfWidth;
    vec3 rright;    float halfHeight;
    vec3 up;        float intensity;
    vec3 color;     uint useVideoTexture;
};
layout(std430, set = 1, binding = 5) readonly buffer RectLightBuf { RectLight rectLights[]; };

// Fog volumes (global height fog + bounded AABB banks). std430 twin of
// Vapor::VolumetricFogVolumeGPU (render_data.hpp / 3d_volumetric_fog.metal).
struct FogVolume {
    vec4 boundsMin;      // xyz world AABB min; w = bounded flag (0 = global, 1 = AABB)
    vec4 boundsMax;      // xyz world AABB max; w = edgeFalloff (world units)
    vec4 densityParams;  // density, heightFalloff, baseHeight, maxHeight
    vec4 albedoBlend;    // albedo.rgb, blendWeight
    vec4 phaseNoise;     // anisotropy, ambientIntensity, noiseScale, noiseIntensity
    vec4 wind;           // windSpeed (× wind strength), ...
};
layout(std430, set = 1, binding = 6) readonly buffer FogVolumeBuf { FogVolume fogVolumes[]; };

// x = cull grid X, y = cull grid Y, z = spot count, w = rect count
// (RHI::setFragmentBytes binding 0 -> push offset 64).
layout(push_constant) uniform PushConstants {
    layout(offset = 64) uvec4 fogLightParams;
};

const float INV_4PI = 0.07957747;

float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    float d = 1.0 + g2 - 2.0 * g * cosTheta;
    return INV_4PI * (1.0 - g2) / pow(d, 1.5);
}

// Value-noise fBm — GLSL port of the Metal 3d_volumetric_common.metal helpers
// so the RHI fog's density noise matches the native simpleFogFragment.
float fogHash13(vec3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}
float fogValueNoise3D(vec3 p) {
    vec3 pi = floor(p);
    vec3 pf = fract(p);
    vec3 w = pf * pf * (3.0 - 2.0 * pf);
    return mix(
        mix(mix(fogHash13(pi + vec3(0,0,0)), fogHash13(pi + vec3(1,0,0)), w.x),
            mix(fogHash13(pi + vec3(0,1,0)), fogHash13(pi + vec3(1,1,0)), w.x), w.y),
        mix(mix(fogHash13(pi + vec3(0,0,1)), fogHash13(pi + vec3(1,0,1)), w.x),
            mix(fogHash13(pi + vec3(0,1,1)), fogHash13(pi + vec3(1,1,1)), w.x), w.y),
        w.z);
}
float fogFbm3D(vec3 p, int octaves, float lacunarity, float gain) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    for (int i = 0; i < octaves; i++) {
        value += amplitude * fogValueNoise3D(p * frequency);
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return value;
}

// Blended fog density at a world position. Densities from every volume ADD
// (extinction is additive across overlapping media); each volume is either
// global exponential height fog or an AABB bank with soft edges, then modulated
// by its own wind-animated value-noise. Falls back to the legacy single-volume
// height fog when no volumes are bound (volumeCount == 0). The lighting still
// uses the fog buffer's (first-volume) phase/ambient — the froxel path (native
// Metal) does the full per-volume material blend.
float blendedDensity(vec3 p) {
    if (volumeCount == 0u) {
        float d = fogDensity * exp(-max(0.0, p.y - fogBaseHeight) * max(fogHeightFalloff, 1e-4));
        if (p.y > fogMaxHeight) d = 0.0;
        if (noiseScale > 0.0) {
            vec3 np = p * noiseScale + windDirection * (time * windSpeed);
            float n = fogFbm3D(np, 4, 2.0, 0.5) * 0.5 + 0.5;
            d = max(0.0, d * (1.0 + (n - 0.5) * noiseIntensity * 2.0));
        }
        return d;
    }
    float total = 0.0;
    for (uint i = 0u; i < volumeCount; ++i) {
        FogVolume v = fogVolumes[i];
        float density = v.densityParams.x;
        float d;
        if (v.boundsMin.w < 0.5) {
            // Global exponential height fog.
            if (p.y > v.densityParams.w) continue;
            d = density * exp(-max(0.0, p.y - v.densityParams.z) * max(v.densityParams.y, 1e-4));
        } else {
            // AABB bank with soft edges.
            vec3 bmin = v.boundsMin.xyz;
            vec3 bmax = v.boundsMax.xyz;
            if (any(lessThan(p, bmin)) || any(greaterThan(p, bmax))) continue;
            vec3 dmn = p - bmin;
            vec3 dmx = bmax - p;
            float edge = min(min(min(dmn.x, dmn.y), dmn.z), min(min(dmx.x, dmx.y), dmx.z));
            d = density * clamp(edge / max(v.boundsMax.w, 1e-4), 0.0, 1.0);
        }
        if (d <= 0.0) continue;
        float nScale = v.phaseNoise.z;
        if (nScale > 0.0) {
            vec3 np = p * nScale + windDirection * (time * v.wind.x);
            float n = fogFbm3D(np, 4, 2.0, 0.5) * 0.5 + 0.5;
            d *= (1.0 + (n - 0.5) * v.phaseNoise.w * 2.0);
        }
        total += max(0.0, d);
    }
    return total;
}

// One-tap PSSM visibility at an arbitrary world position (RHIMain.frag's
// cascade conventions: uv = proj.xy*0.5+0.5, manual depth compare). Positions
// outside every cascade count as lit.
float sunVisibilityAt(vec3 p, float viewDepth) {
    int ci = 0;
    if      (viewDepth > cascadeSplits.z) ci = 2;
    else if (viewDepth > cascadeSplits.y) ci = 1;
    vec4 lp = shadowLightSpaceMatrices[ci] * vec4(p, 1.0);
    vec3 proj = lp.xyz / lp.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z > 1.0) {
        return 1.0;
    }
    float d = texture(pssmShadowMaps, vec3(uv, float(ci))).r;
    return (proj.z - 0.002) <= d ? 1.0 : 0.0;
}

void main() {
    vec4 color = texture(sceneColor, tex_uv);
    float depth = texture(sceneDepth, tex_uv).r;

    if (depth >= 0.9999) { outColor = color; return; }  // sky: no fog (parity)

    // Reconstruct world position (GL-convention +Y-up NDC, Vulkan ZO depth).
    vec2 ndc = vec2(tex_uv.x * 2.0 - 1.0, 1.0 - tex_uv.y * 2.0);
    vec4 wp = invViewProj * vec4(ndc, depth, 1.0);
    vec3 worldPos = wp.xyz / wp.w;

    vec3 rayDir = normalize(worldPos - cameraPosition);
    float marchDist = length(worldPos - cameraPosition);

    // Per-pixel tile light list: the cull writer indexes tiles in y-up screen
    // space, tex_uv is y-down.
    uvec2 grid = fogLightParams.xy;
    uint tileX = min(uint(tex_uv.x * float(grid.x)), grid.x - 1u);
    uint tileY = min(uint((1.0 - tex_uv.y) * float(grid.y)), grid.y - 1u);
    uint tileIdx = tileX + tileY * grid.x;
    uint tileLights = min(tiles[tileIdx].lightCount, 16u);  // fog cost cap

    const int STEPS = 32;
    float stepLen = marchDist / float(STEPS);
    float sunPhase = phaseHG(dot(rayDir, sunDirection), anisotropy);

    vec3 scatter = vec3(0.0);
    float trans = 1.0;
    for (int st = 0; st < STEPS; ++st) {
        float t = (float(st) + 0.5) * stepLen;
        vec3 p = cameraPosition + rayDir * t;
        // Blended density across every fog volume (bounded banks + global haze).
        float dens = blendedDensity(p);
        if (dens < 1e-6) continue;

        vec3 L = vec3(0.0);

        // Sun with volumetric PSSM shadow (light shafts). The march distance t
        // approximates the view depth for cascade selection.
        L += sunColor * sunIntensity * sunPhase * sunVisibilityAt(p, t);

        // Tile-culled point lights (surface-matching falloff).
        for (uint i = 0u; i < tileLights; ++i) {
            PointLight pl = pointLights[tiles[tileIdx].lightIndices[i]];
            vec3 toL = pl.position - p;
            float d2 = max(dot(toL, toL), 1e-4);
            float d = sqrt(d2);
            if (d >= pl.radius) continue;
            float atten = (1.0 - smoothstep(pl.radius * 0.8, pl.radius, d)) / d2;
            L += pl.color * pl.intensity * atten * phaseHG(dot(rayDir, toL / d), anisotropy);
        }

        // Spot cones (loop-all; scenes carry a handful).
        for (uint i = 0u; i < fogLightParams.z; ++i) {
            SpotLight sl = spotLights[i];
            vec3 toL = sl.position - p;
            float d2 = max(dot(toL, toL), 1e-4);
            float d = sqrt(d2);
            if (d >= sl.radius) continue;
            vec3 ldir = toL / d;
            float cone = clamp((dot(-ldir, sl.direction) - sl.cosOuter)
                                   / max(sl.cosInner - sl.cosOuter, 1e-4), 0.0, 1.0);
            if (cone <= 0.0) continue;
            float atten = (1.0 - smoothstep(sl.radius * 0.8, sl.radius, d)) / d2 * cone * cone;
            L += sl.color * sl.intensity * atten * phaseHG(dot(rayDir, ldir), anisotropy);
        }

        // Rect area lights: attenuate from the CLOSEST point on the quad so the
        // scattering hugs the rectangle's extent (a rounded slab near the
        // panel) instead of a spherical center-point glow; far away the clamp
        // collapses to the center and it degenerates to a point light. Two-sided
        // (fog is a coarse scattering approximation, so no front-face cull).
        // std430 tail-packs the scalars, so `rright` avoids the keyword clash.
        for (uint i = 0u; i < fogLightParams.w; ++i) {
            RectLight rl = rectLights[i];
            vec3 rel = p - rl.position;
            float pu = clamp(dot(rel, rl.rright), -rl.halfWidth,  rl.halfWidth);
            float pv = clamp(dot(rel, rl.up),     -rl.halfHeight, rl.halfHeight);
            vec3 closest = rl.position + rl.rright * pu + rl.up * pv;
            float area = 4.0 * rl.halfWidth * rl.halfHeight;
            float range = 8.0 * max(rl.halfWidth, rl.halfHeight);
            vec3 toL = closest - p;
            float d2 = max(dot(toL, toL), 1e-4);
            float d = sqrt(d2);
            if (d >= range) continue;
            float atten = (1.0 - smoothstep(range * 0.8, range, d)) / d2 * area;
            L += rl.color * rl.intensity * atten * phaseHG(dot(rayDir, toL / d), anisotropy);
        }

        // Ambient floor so fog reads even away from direct light.
        L += vec3(0.5, 0.6, 0.7) * ambientIntensity;

        scatter += trans * L * dens * stepLen;
        trans *= exp(-dens * stepLen);
    }

    outColor = vec4(color.rgb * trans + scatter, color.a);
}
