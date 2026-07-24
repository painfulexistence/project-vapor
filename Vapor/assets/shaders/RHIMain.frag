#version 450
#ifdef BINDLESS
#extension GL_EXT_nonuniform_qualifier : require
#endif
// RHI renderer main-pass fragment shader (Vulkan backend).
// Compiled twice by the asset pipeline: plain (RHIMain.frag.spv, per-draw
// material textures at set2 b0-5) and with -DBINDLESS
// (RHIMainBindless.frag.spv, material textures fetched from the set-3 runtime
// array by fragMaterialID — the Bindless MDI draw mode, where one
// vkCmdDrawIndexedIndirect draws the whole scene and per-draw texture binding
// is impossible).
// PBR: direct dir/point lighting, normal mapping, tiled point-light culling
// (clusters), cascaded shadows, and optional IBL (per-material iblEnabled).
//
// Binding convention (see rhi_vulkan.cpp):
//   set 0 = vertex-stage buffers (visible here too; materials live in binding 1)
//   set 1 = fragment-stage buffers
//   set 2 = fragment textures
//   push constants: fragment bytes at offset 64+(binding%4)*16 in [64,128)

// FORCE early depth testing. The main pass loads the pre-pass depth and tests
// LessEqual, so every occluded fragment should be rejected BEFORE this
// (expensive PBR) shader runs. The hand-written Metal PBR shader gets that
// early-Z automatically; MoltenVK's translation of this shader does NOT unless
// the SPIR-V carries the EarlyFragmentTests execution mode — without it the
// Vulkan Main pass pays full overdraw (measured ~25ms vs ~7ms on Metal for the
// identical algorithm on the same GPU). Safe with the alpha-cutout discard
// below ONLY because the pipeline writes no depth (depthWrite=false; the
// pre-pass owns depth): early-Z here tests but never stamps a discarded MASK
// texel's depth, so cutout holes keep the pre-pass depth behind them.
layout(early_fragment_tests) in;

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 worldNormal;
layout(location = 3) in vec4 worldTangent;
layout(location = 4) flat in uint fragMaterialID;
layout(location = 5) in vec4 instanceColor;

layout(location = 0) out vec4 outColor;

// Must match Vapor::MaterialData (C++, std430 stride = 112)
struct MaterialData {
    vec4 baseColorFactor;
    float normalScale;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    vec3 emissiveFactor;
    float alphaCutoff;// MASK-mode cutoff; 0 = disabled
    float emissiveStrength;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
    // Tail matching Vapor::MaterialData (graphics_gpu_structs.hpp, std430 stride
    // 112): iblEnabled gates the IBL indirect term; transmission is Metal-RT-only
    // but MUST stay here or every materials[i>0] read shifts by 16*i bytes.
    float prototypeUVMode;
    float uvScale;
    float iblEnabled;
    float transmission;
    // Surface shader model (0 Standard / 1 Terrain / 2 Grass). In the C++ tail
    // after transmission; std430 stride stays 112.
    float shaderModel;
};

// Must match DirectionalLightData / PointLightData (C++, stride 48 each)
struct DirLight {
    vec3 direction;
    float _pad1;
    vec3 color;
    float _pad2;
    float intensity;
    vec3 _pad3;
};

struct PointLight {
    vec3 position;
    float _pad1;
    vec3 color;
    float _pad2;
    float intensity;
    float radius;
    vec2 _pad3;
};

// Must match Vapor::SpotLight (std430, 64 bytes; scalars follow the vectors).
struct SpotLight {
    vec3 position;
    float _pad0;
    vec3 direction;   // normalized, FROM the light
    float _pad1;
    vec3 color;
    float _pad2;
    float radius;     // range
    float cosInner;
    float cosOuter;
    float intensity;
};

// Must match CameraRenderData (C++)
struct CameraData {
    mat4 proj;
    mat4 view;
    mat4 invProj;
    mat4 invView;
    float nearPlane;
    float farPlane;
    vec2 _pad;
    vec3 position;
    float _pad2;
    vec4 frustumPlanes[6];
};

layout(std430, set = 0, binding = 1) readonly buffer MaterialBuf {
    MaterialData materials[];
};
layout(std430, set = 1, binding = 0) readonly buffer DirLightBuf {
    DirLight dirLights[];
};
layout(std430, set = 1, binding = 1) readonly buffer PointLightBuf {
    PointLight pointLights[];
};
layout(std430, set = 1, binding = 3) readonly buffer CameraBuf {
    CameraData cam;
};
// Tile light culling output (TileLightCull.comp) + its dimensions.
const uint MAX_LIGHTS_PER_TILE = 256;
const uint MAX_SPOTS_PER_CLUSTER = 64u;  // must match graphics.hpp Cluster
const uint MAX_RECTS_PER_CLUSTER = 32u;
struct Cluster {
    vec4 mn;
    vec4 mx;
    uint lightCount;
    uint lightIndices[MAX_LIGHTS_PER_TILE];
    uint spotCount;
    uint spotIndices[MAX_SPOTS_PER_CLUSTER];
    uint rectCount;
    uint rectIndices[MAX_RECTS_PER_CLUSTER];
};
layout(std430, set = 1, binding = 4) readonly buffer ClusterBuf { Cluster tiles[]; };
layout(std430, set = 1, binding = 5) readonly buffer LightCullBuf {
    vec2 cullScreenSize;
    // Weather-driven environment dimming (Vapor::LightCullData.iblIntensity):
    // heavy cloud cover scales the baked IBL down at shading time.
    float cullIblIntensity;
    float _cpad1;
    uvec3 cullGridSize;
    uint cullLightCount;
};
layout(std430, set = 1, binding = 6) readonly buffer SpotLightBuf {
    SpotLight spotLights[];
};

// Must match Vapor::RectLight (std430 legally packs the scalars into the vec3
// tails, unlike MSL — same 64-byte layout as the C++ struct).
struct RectLight {
    vec3 position;  float halfWidth;
    vec3 rright;    float halfHeight;
    vec3 up;        float intensity;
    vec3 color;     uint useVideoTexture;
};
layout(std430, set = 1, binding = 7) readonly buffer RectLightBuf {
    RectLight rectLights[];
};
// PSSM cascaded directional shadow data. Must match Vapor::PSSMRenderData
// (std430). The neutral default (cascadeSplits = +inf) keeps every pixel in the
// nearest cascade sampling the identity matrix, i.e. fully lit until the shadow
// pass runs. cascadeSplits holds view-space forward distances: x = near end,
// w = far end; the three cascades span [x,y], [y,z], [z,w].
layout(std430, set = 1, binding = 2) readonly buffer PSSMBuf {
    mat4 shadowLightSpaceMatrices[3];
    vec4 cascadeSplits;
    float shadowBlendRange;     // 208
    float cascadeBlendRange;    // 212  cascade<->cascade blend width (view units)
    uint pcfSampleCount;        // 216  PCF taps: 4/8/16/32
    uint debugVisualize;        // 220  cascade-colour debug (0 = off)
    float nearShadowEnd;        // 224  view depth the near map covers; 0 = off
    float _pssmPad0;            // 228
    float _pssmPad1;            // 232  (pad so nearLightMatrix is 16-aligned at 240)
    float _pssmPad2;            // 236
    mat4 nearLightMatrix;       // 240  near-field map (own texture, nearShadowTex)
};

// Poisson disk (matches 3d_common.metal) — 4/8/16 use the first N; 32 adds a
// 45deg-rotated second set, mirroring the Metal samplePSSMShadow tap schedule.
const vec2 kPoisson16[16] = vec2[16](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790)
);

#ifndef BINDLESS
layout(set = 2, binding = 0) uniform sampler2D albedoMap;
layout(set = 2, binding = 1) uniform sampler2D normalMap;
layout(set = 2, binding = 2) uniform sampler2D metallicMap;
layout(set = 2, binding = 3) uniform sampler2D roughnessMap;
layout(set = 2, binding = 4) uniform sampler2D occlusionMap;
layout(set = 2, binding = 5) uniform sampler2D emissiveMap;
#else
// Bindless MDI: material textures live in the set-3 runtime array (6 slots per
// material, written by the renderer's bindless table — see
// RHI::createTextureArgumentTable), sampled with one shared trilinear-repeat
// sampler. The macros keep every texture() site below identical to the bound
// path. nonuniformEXT: fragments of different materials run in the same wave.
layout(set = 3, binding = 0) uniform texture2D bindlessTextures[];
layout(set = 3, binding = 1) uniform sampler bindlessSampler;
#define MAT_TEX(slot) sampler2D(bindlessTextures[nonuniformEXT(fragMaterialID * 6u + slot)], bindlessSampler)
#define albedoMap    MAT_TEX(0u)
#define normalMap    MAT_TEX(1u)
#define metallicMap  MAT_TEX(2u)
#define roughnessMap MAT_TEX(3u)
#define occlusionMap MAT_TEX(4u)
#define emissiveMap  MAT_TEX(5u)
#endif
layout(set = 2, binding = 6) uniform sampler2DArray shadowMap;  // 3-cascade depth array
// Screen-space AO (SSAO chain output; white texture when AO is disabled).
// Attenuates ambient/indirect light ONLY — multiplying direct light by AO is
// physically wrong (same rule as the Metal PBR shader).
layout(set = 2, binding = 7) uniform sampler2D texAO;
// Screen-space contact shadow visibility (1 = lit); min-composited onto the sun
// shadow. White when disabled. Screen-space, sampled at gl_FragCoord like texAO.
layout(set = 2, binding = 8) uniform sampler2D sscsTex;
// Independent near-field shadow map (own texture + resolution), covers
// [near, nearShadowEnd]. Depth values sampled with the negative-viewport Y-flip.
layout(set = 2, binding = 9) uniform sampler2D nearShadowTex;
// IBL from the sky bake (matches the Metal path): diffuse irradiance cube,
// GGX-prefiltered specular cube, split-sum BRDF LUT. Bindings 10-12 sit clear
// of sscsTex(8)/nearShadowTex(9), so no sampler-type aliasing (which would
// break MoltenVK's SPIR-V->MSL translation). Bound to black defaults when no
// environment is loaded; sampled only when MaterialData.iblEnabled != 0.
layout(set = 2, binding = 10) uniform samplerCube irradianceMap;
layout(set = 2, binding = 11) uniform samplerCube prefilterMap;
layout(set = 2, binding = 12) uniform sampler2D brdfLut;
// Terrain detail layers (grass/rock/dirt/snow), world-space tiled: two arrays
// (4 albedo, 4 tangent-space normal) SHARED by every terrain tile — bound once
// per frame (default white when no terrain is staged), sampled only by the
// shaderModel == 1 (Terrain) branch. b13/b14 are the slots the raised
// TEXTURE_BINDINGS_PER_SET added; a plain Standard draw keeps them bound (to the
// default) but never samples them.
layout(set = 2, binding = 13) uniform sampler2DArray terrainDetailAlbedo;
layout(set = 2, binding = 14) uniform sampler2DArray terrainDetailNormal;

// PCF sample of one cascade. Returns 1.0 = lit, 0.0 = fully shadowed, or -1.0
// when the world position falls outside this cascade's frustum.
float sampleCascade(int ci, vec3 worldPos, float bias) {
    vec4 lp = shadowLightSpaceMatrices[ci] * vec4(worldPos, 1.0);
    vec3 proj = lp.xyz / lp.w;
    // The shadow map is rendered through the engine's negative-height viewport
    // (rhi_vulkan: viewport.height = -H), which rasterizes ndc.y=+1 to texture
    // row 0. So the sample UV must flip Y (v = 0.5 - 0.5*proj.y); using
    // 0.5 + 0.5*proj.y samples the vertically-mirrored texel, which reads
    // correctly only at the cascade centre and makes shadows drift/swim as the
    // camera (and thus the cascade centre) moves. z stays [0,1] (ZO).
    vec2 uv = vec2(proj.x * 0.5 + 0.5, 0.5 - proj.y * 0.5);
    float curDepth = proj.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || curDepth > 1.0) {
        return -1.0;
    }
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float refDepth = curDepth - bias;
    uint n = clamp(pcfSampleCount, 4u, 16u);
    float lit = 0.0;
    for (uint i = 0u; i < n; ++i) {
        float d = texture(shadowMap, vec3(uv + kPoisson16[i] * texel * 2.0, float(ci))).r;
        lit += refDepth <= d ? 1.0 : 0.0;
    }
    float total = float(n);
    if (pcfSampleCount >= 32u) {          // second, rotated 16-tap set
        for (uint i = 0u; i < 16u; ++i) {
            vec2 r = vec2(kPoisson16[i].x - kPoisson16[i].y,
                          kPoisson16[i].x + kPoisson16[i].y) * 0.7071 * 1.5;
            float d = texture(shadowMap, vec3(uv + r * texel * 2.0, float(ci))).r;
            lit += refDepth <= d ? 1.0 : 0.0;
        }
        total += 16.0;
    }
    return lit / total;
}

// Independent near-field shadow map (own texture, own resolution): a tight, high-
// effective-resolution fit for [near, nearShadowEnd]. Same negative-viewport
// Y-flip as the cascades. Returns -1.0 when the fragment is outside its frustum.
float sampleNearMap(vec3 worldPos, float bias) {
    vec4 lp = nearLightMatrix * vec4(worldPos, 1.0);
    vec3 proj = lp.xyz / lp.w;
    vec2 uv = vec2(proj.x * 0.5 + 0.5, 0.5 - proj.y * 0.5);
    float curDepth = proj.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || curDepth > 1.0) {
        return -1.0;
    }
    vec2 texel = 1.0 / vec2(textureSize(nearShadowTex, 0).xy);
    float refDepth = curDepth - bias;
    uint n = clamp(pcfSampleCount, 4u, 16u);
    float lit = 0.0;
    for (uint i = 0u; i < n; ++i) {
        float d = texture(nearShadowTex, uv + kPoisson16[i] * texel * 2.0).r;
        lit += refDepth <= d ? 1.0 : 0.0;
    }
    float total = float(n);
    if (pcfSampleCount >= 32u) {
        for (uint i = 0u; i < 16u; ++i) {
            vec2 r = vec2(kPoisson16[i].x - kPoisson16[i].y,
                          kPoisson16[i].x + kPoisson16[i].y) * 0.7071 * 1.5;
            float d = texture(nearShadowTex, uv + r * texel * 2.0).r;
            lit += refDepth <= d ? 1.0 : 0.0;
        }
        total += 16.0;
    }
    return lit / total;
}

// Select the near-field map or a cascade by the fragment's view-space depth,
// PCF-sample it, and cross-fade across boundaries so transitions don't seam.
// Returns 1.0 = lit, 0.0 = fully shadowed. Boundaries (view depth):
//   nearShadowEnd = near map -> cascade 0,  cascadeSplits.y = c0 -> c1,
//   cascadeSplits.z = c1 -> c2. Blend band width = cascadeBlendRange.
float sampleShadow(vec3 worldPos, vec3 N, vec3 L, float viewDepth) {
    float b = max(0.0015 * (1.0 - dot(N, L)), 0.0004);
    float blend = max(cascadeBlendRange, 1e-4);

    // Near field: dedicated high-res map, cross-fading into cascade 0 near its far edge.
    if (nearShadowEnd > 0.0 && viewDepth < nearShadowEnd) {
        float nearLit = sampleNearMap(worldPos, b);
        if (nearLit >= 0.0) {
            if (viewDepth > nearShadowEnd - blend) {
                float c0 = sampleCascade(0, worldPos, b);
                if (c0 >= 0.0) {
                    float t = (viewDepth - (nearShadowEnd - blend)) / blend;
                    return mix(nearLit, c0, smoothstep(0.0, 1.0, t));
                }
            }
            return nearLit;
        }
        // outside the near frustum -> fall through to the cascades
    }

    // Pick the first cascade whose far split still contains this fragment.
    int ci = 2;
    if (viewDepth <= cascadeSplits.y)      ci = 0;
    else if (viewDepth <= cascadeSplits.z) ci = 1;
    float lit = sampleCascade(ci, worldPos, b * float(ci + 1));
    // Fall through to farther cascades if the fragment left this one's frustum.
    if (lit < 0.0 && ci < 1) { ci = 1; lit = sampleCascade(1, worldPos, b * 2.0); }
    if (lit < 0.0 && ci < 2) { ci = 2; lit = sampleCascade(2, worldPos, b * 3.0); }
    if (lit < 0.0) return 1.0;  // outside all cascades -> lit

    // Cross-fade into the next cascade near this cascade's far split.
    if (ci < 2) {
        float farSplit = (ci == 0) ? cascadeSplits.y : cascadeSplits.z;
        if (viewDepth > farSplit - blend) {
            float nextLit = sampleCascade(ci + 1, worldPos, b * float(ci + 2));
            if (nextLit >= 0.0) {
                float t = (viewDepth - (farSplit - blend)) / blend;
                lit = mix(lit, nextLit, smoothstep(0.0, 1.0, t));
            }
        }
    }
    return lit;
}

// RHI::setFragmentBytes(&dirLightCount, 4, /*binding=*/11) -> offset 64+(11%4)*16 = 112
// RHI::setFragmentBytes(&screenSize, 8, /*binding=*/4)     -> offset 64+(4%4)*16  = 64
layout(push_constant) uniform PushConstants {
    layout(offset = 64)  vec2 screenSize;   // swapchain pixels (for AO screen UV)
    // Spot-light loop bound (setFragmentBytes binding=1 -> offset 80).
    layout(offset = 80)  uvec2 spotRectCounts;  // x = spot count, y = rect count
    // Perf-isolation debug flags (setFragmentBytes binding=2 -> offset 96).
    // bit0 = skip the point-light loop, bit1 = skip the shadow PCF. Panel-driven.
    layout(offset = 96)  uint mainDebugFlags;
    // Directional-light loop bound. (Was uvec2 lightCounts; the .y point count
    // has been dead since the tile-cull port — the point loop reads per-cluster
    // counts from the Cluster buffer.)
    layout(offset = 112) uint dirLightCount;
};

const float PI = 3.14159265359;

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-5);
}

float geometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float g1 = NdotV / (NdotV * (1.0 - k) + k);
    float g2 = NdotL / (NdotL * (1.0 - k) + k);
    return g1 * g2;
}

// Roughness-aware Fresnel for the IBL ambient term.
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ── Rect area light (Vulkan twin of the Metal eval) ─────────────────────────

// Exact diffuse irradiance from a quad via the polygon solid-angle edge
// formula (Baum et al. 1989 / Arvo 1994). Returns irradiance in [0, 1/2].
float rectLightDiffuse(vec3 N, vec3 fragPos, RectLight l) {
    vec3 corners[4] = vec3[4](
        l.position + l.rright * l.halfWidth + l.up * l.halfHeight,
        l.position - l.rright * l.halfWidth + l.up * l.halfHeight,
        l.position - l.rright * l.halfWidth - l.up * l.halfHeight,
        l.position + l.rright * l.halfWidth - l.up * l.halfHeight);
    vec3 s = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        vec3 v0 = normalize(corners[i] - fragPos);
        vec3 v1 = normalize(corners[(i + 1) % 4] - fragPos);
        vec3 c = cross(v0, v1);
        float len = length(c);
        if (len < 1e-6) continue;
        float theta = atan(len, dot(v0, v1));
        s += (theta / len) * c;
    }
    return max(0.0, dot(s, N)) / (2.0 * PI);
}

// Specular via Most Representative Point (Karis, SIGGRAPH 2013): closest point
// on the rect to the reflection ray, GGX with area-corrected roughness.
vec3 rectLightSpecular(vec3 N, vec3 V, vec3 fragPos, RectLight l,
                       vec3 albedo, float metallic, float roughness) {
    vec3 refl = reflect(-V, N);
    vec3 lightNorm = cross(l.rright, l.up);  // unnormalized plane normal
    float denom = dot(lightNorm, refl);
    vec3 repPt = l.position;
    if (abs(denom) > 1e-5) {
        float t = dot(l.position - fragPos, lightNorm) / denom;
        if (t > 0.0) {
            vec3 hit = fragPos + refl * t;
            vec3 rel = hit - l.position;
            float u = clamp(dot(rel, l.rright), -l.halfWidth, l.halfWidth);
            float v = clamp(dot(rel, l.up), -l.halfHeight, l.halfHeight);
            repPt = l.position + l.rright * u + l.up * v;
        }
    }
    vec3 L = normalize(repPt - fragPos);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);
    float dist = length(repPt - fragPos);
    float area = 4.0 * l.halfWidth * l.halfHeight;
    float alpha = roughness * roughness;
    float alphaPrime = clamp(alpha + area / max(2.0 * PI * dist * dist, 1e-6), 0.0, 1.0);
    float r = sqrt(alphaPrime);
    vec3 H = normalize(L + V);
    float NdotV = max(dot(N, V), 1e-4);
    float D = distributionGGX(N, H, r);
    float G = geometrySmith(NdotV, NdotL, r);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    return (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
}

// ── Disney principled BRDF ──────────────────────────────────────────────────
// GLSL twin of 3d_pbr_normal_mapped.metal's CookTorranceBRDF, so the Vulkan
// direct lighting matches the Metal look: retro-reflective diffuse + subsurface,
// anisotropic GGX specular, plus sheen and clearcoat lobes. Returns f_r * NdotL
// (the cosine is folded in exactly ONCE — the callers must not multiply again).
struct Surface {
    vec3  color;
    float roughness;
    float metallic;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
};

float luminance(vec3 c)     { return dot(c, vec3(0.3, 0.6, 0.1)); }
float fresnelApprox(float u) { return pow(1.0001 - u, 5.0); }

// Exact sRGB -> linear (matches 3d_common.metal's srgbToLinear). Albedo
// textures are UNORM (raw sRGB bytes), so lighting must linearize them — Metal
// does this per pixel; without it the Vulkan path lit with sRGB values, which
// read too bright.
vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

float GTR1(float nh, float a) {
    if (a >= 1.0) return 1.0 / PI;
    float a2 = a * a;
    float t = 1.0 + (a2 - 1.0) * nh * nh;
    return (a2 - 1.0) / (PI * log(a2) * t);
}
float GTR2aniso(float nh, float hx, float hy, float ax, float ay) {
    float t = (hx * hx) / (ax * ax) + (hy * hy) / (ay * ay) + nh * nh;
    return 1.0 / (PI * ax * ay * t * t);
}
float smithGGX(float u, float r) {
    float a = r * r, b = u * u;
    return 1.0 / (u + sqrt(a + b - a * b));
}
float smithGGXaniso(float u, float vx, float vy, float ax, float ay) {
    float t = vx * vx * ax * ax + vy * vy * ay * ay + u * u;
    return 1.0 / (u + sqrt(t));
}

vec3 disneyBRDF(vec3 N, vec3 T, vec3 B, vec3 L, vec3 V, Surface s) {
    vec3 H = normalize(L + V);
    float nv = max(dot(N, V), 0.0);
    float nl = max(dot(N, L), 0.0);
    float nh = max(dot(N, H), 0.0);
    float lh = max(dot(L, H), 0.0);
    float lum = luminance(s.color);
    vec3 tint = lum > 0.0 ? s.color / lum : vec3(1.0);
    vec3 spec0 = mix(s.specular * 0.08 * mix(vec3(1.0), tint, s.specularTint), s.color, s.metallic);
    float fh = fresnelApprox(lh);
    float fl = fresnelApprox(nl);
    float fv = fresnelApprox(nv);
    float fss90 = lh * lh * s.roughness;
    // Disney retro-reflective diffuse
    float fd90 = 0.5 + 2.0 * fss90;
    float kd = mix(1.0, fd90, fl) * mix(1.0, fd90, fv);
    // subsurface approximation
    float fss = mix(1.0, fss90, fl) * mix(1.0, fss90, fv);
    float ss = 1.25 * (fss * (1.0 / (nl + nv + 0.0001) - 0.5) + 0.5);
    // anisotropic GGX specular
    float aspect = sqrt(1.0 - s.anisotropic * 0.9);
    float ax = max(0.001, s.roughness * s.roughness / aspect);
    float ay = max(0.001, s.roughness * s.roughness * aspect);
    float hx = dot(H, T), hy = dot(H, B);
    float lx = dot(L, T), ly = dot(L, B);
    float vx = dot(V, T), vy = dot(V, B);
    float D = GTR2aniso(nh, hx, hy, ax, ay);
    float G = smithGGXaniso(nl, lx, ly, ax, ay) * smithGGXaniso(nv, vx, vy, ax, ay);
    vec3  F = mix(spec0, vec3(1.0), fh);
    vec3  specular = D * G * F;
    // sheen
    vec3 sheen = fh * s.sheen * mix(vec3(1.0), tint, s.sheenTint);
    // clearcoat
    float Dr = GTR1(nh, mix(0.1, 0.001, s.clearcoatGloss));
    float Fr = mix(0.04, 1.0, fh);
    float Gr = smithGGX(nl, 0.25) * smithGGX(nv, 0.25);
    vec3  clearcoat = 0.25 * vec3(s.clearcoat) * Dr * Fr * Gr;

    return ((mix(kd, ss, s.subsurface) * s.color / PI + sheen) * (1.0 - s.metallic)
            + specular + clearcoat) * nl;
}

// ── Image-based lighting (twin of Metal's CalculateIBL) ─────────────────────
vec3 calculateIBL(vec3 N, vec3 V, Surface s, float ao) {
    float NdotV = max(dot(N, V), 0.0);
    vec3 F0 = mix(vec3(0.04), s.color, s.metallic);
    vec3 F = fresnelSchlickRoughness(NdotV, F0, s.roughness);
    vec3 kD = (1.0 - F) * (1.0 - s.metallic);
    // Diffuse IBL
    vec3 diffuseIBL = texture(irradianceMap, N).rgb * s.color * kD;
    // Specular IBL (split-sum): prefiltered env by roughness + BRDF LUT
    vec3 R = reflect(-V, N);
    const float MAX_REFLECTION_LOD = 4.0;
    vec3 prefiltered = textureLod(prefilterMap, R, s.roughness * MAX_REFLECTION_LOD).rgb;
    vec2 brdf = texture(brdfLut, vec2(NdotV, s.roughness)).rg;
    vec3 specularIBL = prefiltered * (F * brdf.x + brdf.y);
    // cullIblIntensity: weather dims the baked environment under heavy cloud.
    return (diffuseIBL + specularIBL) * ao * cullIblIntensity;
}

// ── Terrain surface (shaderModel == 1) ──────────────────────────────────────
// Faithful port of Atmospheric's terrain.frag (4-layer albedo/normal blend) +
// terrain_texture_gen defaultSplat (weights), but the weights are recomputed
// PER-FRAGMENT from height/slope + world-space FBm breakup — no splat texture,
// so it stays compatible with the fixed-slot terrain streaming. Detail layers
// tile in WORLD space (continuous across streamed tiles). Per-layer world-space
// frequency (repeats/metre): grass 4 m, rock ~21 m, dirt 8 m, snow ~13 m.
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

// ── Terrain height field — FastNoiseLite v1.1.1 OpenSimplex2 FBm, ported
// function-for-function from the vendored header so per-pixel normals
// reconstruct the SAME field the streamed mesh (TerrainWorld::heightAt) is
// built on — which is itself the exact height source of Atmospheric's
// TerrainStreamer. The LOD mesh only carries vertex normals at its grid
// spacing (8 m at LOD0), so every octave finer than that is smoothed away by
// interpolation; re-evaluating the field around each pixel reconstructs the
// fine normal continuously. Signed-int hashing relies on SPIR-V's defined
// two's-complement wrapping. Params arrive packed in the terrain material's
// unused Disney lobe fields (see the terrain material upload in renderer.cpp).
const int kFnlPrimeX = 501125321;
const int kFnlPrimeY = 1136930381;
// Gradients2D is 128 pairs: a 24-direction fan (15 deg steps from 82.5 deg)
// repeated 5 times, then 8 picks at 45 deg steps (fan indices 1,4,7,...,22).
const vec2 kFnlFan[24] = vec2[24](
    vec2(0.130526192220052, 0.99144486137381),   vec2(0.38268343236509, 0.923879532511287),
    vec2(0.608761429008721, 0.793353340291235),  vec2(0.793353340291235, 0.608761429008721),
    vec2(0.923879532511287, 0.38268343236509),   vec2(0.99144486137381, 0.130526192220051),
    vec2(0.99144486137381, -0.130526192220051),  vec2(0.923879532511287, -0.38268343236509),
    vec2(0.793353340291235, -0.60876142900872),  vec2(0.608761429008721, -0.793353340291235),
    vec2(0.38268343236509, -0.923879532511287),  vec2(0.130526192220052, -0.99144486137381),
    vec2(-0.130526192220052, -0.99144486137381), vec2(-0.38268343236509, -0.923879532511287),
    vec2(-0.608761429008721, -0.793353340291235),vec2(-0.793353340291235, -0.608761429008721),
    vec2(-0.923879532511287, -0.38268343236509), vec2(-0.99144486137381, -0.130526192220052),
    vec2(-0.99144486137381, 0.130526192220051),  vec2(-0.923879532511287, 0.38268343236509),
    vec2(-0.793353340291235, 0.608761429008721), vec2(-0.608761429008721, 0.793353340291235),
    vec2(-0.38268343236509, 0.923879532511287),  vec2(-0.130526192220052, 0.99144486137381));
vec2 fnlGradient2(int pairIndex) {
    return pairIndex < 120 ? kFnlFan[pairIndex % 24] : kFnlFan[1 + 3 * (pairIndex - 120)];
}
float fnlGradCoord(int seed, int xPrimed, int yPrimed, float xd, float yd) {
    int hash = (seed ^ xPrimed ^ yPrimed) * 0x27d4eb2d;
    hash ^= hash >> 15;
    hash &= 127 << 1;
    vec2 g = fnlGradient2(hash >> 1);
    return xd * g.x + yd * g.y;
}
// SingleSimplex (2D OpenSimplex2): input is already frequency-scaled + skewed.
float fnlSimplex2(int seed, vec2 p) {
    const float SQRT3 = 1.7320508075688772935;
    const float G2 = (3.0 - SQRT3) / 6.0;
    int i = int(floor(p.x)), j = int(floor(p.y));
    float xi = p.x - float(i), yi = p.y - float(j);
    float t = (xi + yi) * G2;
    float x0 = xi - t, y0 = yi - t;
    i *= kFnlPrimeX;
    j *= kFnlPrimeY;
    float n0 = 0.0, n1 = 0.0, n2 = 0.0;
    float a = 0.5 - x0 * x0 - y0 * y0;
    if (a > 0.0) n0 = (a * a) * (a * a) * fnlGradCoord(seed, i, j, x0, y0);
    float c = (2.0 * (1.0 - 2.0 * G2) * (1.0 / G2 - 2.0)) * t
            + ((-2.0 * (1.0 - 2.0 * G2) * (1.0 - 2.0 * G2)) + a);
    if (c > 0.0) {
        float x2 = x0 + (2.0 * G2 - 1.0), y2 = y0 + (2.0 * G2 - 1.0);
        n2 = (c * c) * (c * c) * fnlGradCoord(seed, i + kFnlPrimeX, j + kFnlPrimeY, x2, y2);
    }
    if (y0 > x0) {
        float x1 = x0 + G2, y1 = y0 + (G2 - 1.0);
        float b = 0.5 - x1 * x1 - y1 * y1;
        if (b > 0.0) n1 = (b * b) * (b * b) * fnlGradCoord(seed, i, j + kFnlPrimeY, x1, y1);
    } else {
        float x1 = x0 + (G2 - 1.0), y1 = y0 + G2;
        float b = 0.5 - x1 * x1 - y1 * y1;
        if (b > 0.0) n1 = (b * b) * (b * b) * fnlGradCoord(seed, i + kFnlPrimeX, j, x1, y1);
    }
    return (n0 + n1 + n2) * 99.83685446303647;
}
// GetNoise: TransformNoiseCoordinate (frequency + F2 skew) -> GenFractalFBm
// (lacunarity 2, gain 0.5, weightedStrength 0), then the heightFn mapping
// noise * 0.5 + 0.5 clamped to [0,1] and scaled — matching heightAt exactly.
float trHeightAt(vec2 xz, float noiseFreq, int octaves, uint seed, float heightScale) {
    vec2 p = xz * noiseFreq;
    const float SQRT3 = 1.7320508075688772935;
    const float F2 = 0.5 * (SQRT3 - 1.0);
    p += vec2((p.x + p.y) * F2);
    const float gain = 0.5;
    float amp = gain, ampFractal = 1.0;
    for (int i = 1; i < octaves; ++i) { ampFractal += amp; amp *= gain; }
    int s = int(seed);
    float sum = 0.0;
    amp = 1.0 / ampFractal;  // fractalBounding
    for (int i = 0; i < octaves; ++i) {
        sum += fnlSimplex2(s++, p) * amp;
        p *= 2.0;
        amp *= gain;
    }
    return clamp(sum * 0.5 + 0.5, 0.0, 1.0) * heightScale;
}

// Fill albedo (linear) + per-pixel world normal from the detail layers. The
// base surface normal is reconstructed from the terrain height field per
// fragment (trHeightAt), not the coarse interpolated vertex normal.
void shadeTerrain(vec3 worldPos, MaterialData mat, float height01, out vec3 albedo, out vec3 N) {
    // Height-field descriptor packed into the terrain material's spare fields.
    float noiseFreq   = mat.subsurface;
    float heightScale = mat.specular;
    int   octaves     = int(mat.specularTint + 0.5);
    uint  seed        = floatBitsToUint(mat.anisotropic);

    // Central-difference normal. The sample distance d tracks the pixel's
    // world-space footprint (>= 1 m), so distant terrain band-limits the noise
    // the way a mipmapped heightmap would — no shimmer — while near terrain
    // resolves the finest octave. Sign convention matches buildTileGeometry's
    // vertex normal: normalize(vec3(hl - hr, 2d, hb - ht)).
    float fp = max(max(abs(dFdx(worldPos.x)), abs(dFdy(worldPos.x))),
                   max(abs(dFdx(worldPos.z)), abs(dFdy(worldPos.z))));
    float d = clamp(fp, 1.0, 64.0);
    float hl = trHeightAt(worldPos.xz - vec2(d, 0.0), noiseFreq, octaves, seed, heightScale);
    float hr = trHeightAt(worldPos.xz + vec2(d, 0.0), noiseFreq, octaves, seed, heightScale);
    float hb = trHeightAt(worldPos.xz - vec2(0.0, d), noiseFreq, octaves, seed, heightScale);
    float ht = trHeightAt(worldPos.xz + vec2(0.0, d), noiseFreq, octaves, seed, heightScale);
    vec3 baseN = normalize(vec3(hl - hr, 2.0 * d, hb - ht));

    float slope = length(baseN.xz) / max(baseN.y, 1e-3);  // rise/run
    vec4 w = terrainSplatWeights(height01, slope, worldPos.xz);
    vec2 wp = worldPos.xz;
    vec3 c = vec3(0.0);
    vec3 dn = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        vec2 uv = wp * kTerrainLayerFreq[i];
        c  += w[i] * pow(texture(terrainDetailAlbedo, vec3(uv, float(i))).rgb, vec3(2.2));
        dn += w[i] * (texture(terrainDetailNormal, vec3(uv, float(i))).xyz * 2.0 - 1.0);
    }
    albedo = c;
    dn = normalize(dn + vec3(0.0, 0.0, 1e-4));
    // perturb the procedural surface normal by the tangent-space detail normal
    vec3 nn = baseN;
    vec3 T = normalize(vec3(1.0, 0.0, 0.0) - nn * nn.x);
    vec3 B = cross(nn, T);
    N = normalize(T * dn.x + B * dn.y + nn * dn.z);
}

void main() {
    MaterialData mat = materials[fragMaterialID];

    vec4 baseSample = texture(albedoMap, fragUV);
    // Alpha cutout (glTF MASK mode — foliage, fences): discard below the
    // cutoff. Must match PrePass.frag's discard or the depth prepass would
    // leave solid quads in the depth buffer where the holes are.
    if (mat.alphaCutoff > 0.0 && baseSample.a * mat.baseColorFactor.a < mat.alphaCutoff) discard;
    // Linearize the sRGB-authored albedo before lighting (mirrors Metal's
    // srgbToLinear(baseColor * baseColorFactor)); instanceColor is a linear tint.
    vec3 albedo = srgbToLinear(baseSample.rgb * mat.baseColorFactor.rgb) * instanceColor.rgb;
    float metallic = texture(metallicMap, fragUV).b * mat.metallicFactor;
    float roughness = clamp(texture(roughnessMap, fragUV).g * mat.roughnessFactor, 0.04, 1.0);
    float occlusion = mix(1.0, texture(occlusionMap, fragUV).r, mat.occlusionStrength);
    // Emissive is sRGB-authored (like albedo) — linearize before it joins the
    // linear lighting sum (mirrors the Metal path).
    vec3 emissive = srgbToLinear(texture(emissiveMap, fragUV).rgb * mat.emissiveFactor) * mat.emissiveStrength;

    // Normal mapping (fall back to the geometric normal when tangent is degenerate)
    vec3 N = normalize(worldNormal);
    vec3 T = worldTangent.xyz;
    vec3 B;
    if (dot(T, T) > 1e-8) {
        T = normalize(T - dot(T, N) * N);
        B = cross(N, T) * worldTangent.w;
        vec3 nSample = texture(normalMap, fragUV).xyz * 2.0 - 1.0;
        nSample.xy *= mat.normalScale;
        N = normalize(mat3(T, B, N) * nSample);
    }

    // Terrain: replace albedo + normal with the world-space detail-layer splat.
    // fragUV.x carries height01 (baked by buildTileGeometry); the geometric
    // normal drives slope. Terrain shades as a rough dielectric through the same
    // PBR lighting below (shadows / lights / IBL), so it is lit consistently
    // with the rest of the scene rather than the original's flat sun term.
    if (mat.shaderModel == 1.0) {
        vec3 tAlbedo, tN;
        shadeTerrain(fragPos, mat, clamp(fragUV.x, 0.0, 1.0), tAlbedo, tN);
        albedo = tAlbedo * instanceColor.rgb;  // detail albedo is already linear
        N = tN;
        metallic = 0.0;
        roughness = 0.95;
    }
    // Orthonormal tangent frame against the (possibly mapped) N for the
    // anisotropic BRDF term. Degenerate tangent -> arbitrary basis (anisotropic
    // defaults to 0, so the exact axis is moot then).
    if (dot(worldTangent.xyz, worldTangent.xyz) > 1e-8) {
        T = normalize(T - dot(T, N) * N);
        B = cross(N, T) * worldTangent.w;
    } else {
        vec3 up = abs(N.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        T = normalize(cross(up, N));
        B = cross(N, T);
    }

    vec3 V = normalize(cam.position - fragPos);

    // Full Disney material for the direct-lighting BRDF (mirrors Metal's Surface).
    // Terrain (shaderModel == 1) overloads the Disney lobe fields to carry its
    // height-field descriptor (see shadeTerrain), so feed the BRDF neutral
    // dielectric values there instead of the packed data — otherwise specular
    // would read heightScale (~500) and anisotropic a garbage seed-bit float.
    // Terrain is meant to shade as a plain rough dielectric anyway.
    bool isTerrain = (mat.shaderModel == 1.0);
    Surface surf;
    surf.color = albedo;
    surf.roughness = roughness;
    surf.metallic = metallic;
    surf.subsurface = isTerrain ? 0.0 : mat.subsurface;
    surf.specular = isTerrain ? 0.5 : mat.specular;
    surf.specularTint = isTerrain ? 0.0 : mat.specularTint;
    surf.anisotropic = isTerrain ? 0.0 : mat.anisotropic;
    surf.sheen = mat.sheen;
    surf.sheenTint = mat.sheenTint;
    surf.clearcoat = mat.clearcoat;
    surf.clearcoatGloss = mat.clearcoatGloss;

    // View-space forward distance selects the shadow cascade (RH: forward = -z).
    float viewDepth = -(cam.view * vec4(fragPos, 1.0)).z;

    vec3 color = vec3(0.0);
    for (uint i = 0u; i < dirLightCount; ++i) {
        DirLight l = dirLights[i];
        vec3 Ldir = normalize(-l.direction);
        vec3 contrib = disneyBRDF(N, T, B, Ldir, V, surf) * (l.color * l.intensity);
        // Only the first (sun) directional light casts the cascaded shadow.
        // Debug bit1 skips the PCF to isolate its cost.
        if (i == 0u && (mainDebugFlags & 2u) == 0u) {
            float sh = sampleShadow(fragPos, N, Ldir, viewDepth);
            // Contact shadows tighten the near contact the cascade/near map miss.
            // min() = shadowed if either says so (no double-darkening from multiply).
            sh = min(sh, texture(sscsTex, gl_FragCoord.xy / max(screenSize, vec2(1.0))).r);
            contrib *= sh;
        }
        color += contrib;
    }
    // Point lights via the culled tile list. The tile is selected with the
    // exact projection math the culling shader uses, so screen-space
    // conventions cancel out by construction. Debug bit0 skips the whole loop.
    // Shared 3D-cluster index for the culled point/spot/rect loops below,
    // computed with the culler's exact projection math so screen-space
    // conventions cancel by construction.
    vec4 clip = cam.proj * cam.view * vec4(fragPos, 1.0);
    vec2 suv = clamp((clip.xy / max(clip.w, 1e-4)) * 0.5 + 0.5, vec2(0.0), vec2(0.9999));
    uvec2 tile = uvec2(suv * vec2(cullGridSize.xy));
    // 3D cluster: logarithmic depth slice, same mapping the culler writes:
    // slice k spans [near*(far/near)^(k/Z), near*(far/near)^((k+1)/Z)).
    // clip.w is the fragment's view-space depth (== viewDepth above).
    uint tileZ = uint(clamp(
        log(max(clip.w, cam.nearPlane) / cam.nearPlane)
            / log(cam.farPlane / cam.nearPlane) * float(cullGridSize.z),
        0.0, float(cullGridSize.z - 1u)));
    uint tileIndex = tile.x + tile.y * cullGridSize.x
                   + tileZ * cullGridSize.x * cullGridSize.y;
    if ((mainDebugFlags & 1u) == 0u) {
        uint count = min(tiles[tileIndex].lightCount, MAX_LIGHTS_PER_TILE);
        for (uint t = 0u; t < count; ++t) {
            PointLight l = pointLights[tiles[tileIndex].lightIndices[t]];
            vec3 toLight = l.position - fragPos;
            float dist2 = max(dot(toLight, toLight), 1e-4);
            // Inverse-square falloff windowed to the light radius (same as the
            // native Metal PBR shader). The cutoff must match the culling
            // radius: without it the light's influence extends past the tiles
            // the culler assigned it to, and every light shows up as a hard
            // bright rectangle.
            float dist = sqrt(dist2);
            float attenuation = (1.0 - smoothstep(l.radius * 0.8, l.radius, dist)) / dist2;
            vec3 radiance = l.color * l.intensity * attenuation;
            color += disneyBRDF(N, T, B, normalize(toLight), V, surf) * radiance;
        }
    }

    // Spot lights (loop-all; typical scenes carry a handful). Same falloff as
    // point lights, windowed by a squared smooth cone factor between the inner
    // and outer half-angle cosines. Unshadowed on Vulkan until RT lands here.
    uint clusterSpots = min(tiles[tileIndex].spotCount, MAX_SPOTS_PER_CLUSTER);
    for (uint sSlot = 0u; sSlot < clusterSpots; ++sSlot) {
        uint sIdx = tiles[tileIndex].spotIndices[sSlot];
        if (sIdx >= spotRectCounts.x) continue;  // culler/frame mismatch guard
        SpotLight sl = spotLights[sIdx];
        vec3 toLight = sl.position - fragPos;
        float dist2 = max(dot(toLight, toLight), 1e-4);
        float dist = sqrt(dist2);
        vec3 Ldir = toLight / dist;
        float cone = clamp((dot(-Ldir, sl.direction) - sl.cosOuter)
                               / max(sl.cosInner - sl.cosOuter, 1e-4), 0.0, 1.0);
        float attenuation = (1.0 - smoothstep(sl.radius * 0.8, sl.radius, dist)) / dist2
                          * cone * cone;
        vec3 radiance = sl.color * sl.intensity * attenuation;
        color += disneyBRDF(N, T, B, Ldir, V, surf) * radiance;
    }

    // Rect area lights (loop-all): analytic diffuse + specular, combined the
    // same way as the Metal CalculateRectLight. Unshadowed on Vulkan (the RT
    // area shadow needs the raytracing port); video-textured lights fall back
    // to their solid color (no video texture bound on this path).
    uint clusterRects = min(tiles[tileIndex].rectCount, MAX_RECTS_PER_CLUSTER);
    for (uint rSlot = 0u; rSlot < clusterRects; ++rSlot) {
        uint rIdx = tiles[tileIndex].rectIndices[rSlot];
        if (rIdx >= spotRectCounts.y) continue;  // culler/frame mismatch guard
        RectLight rl = rectLights[rIdx];
        vec3 radiance = rl.color * rl.intensity;
        float diffuseGeo = rectLightDiffuse(N, fragPos, rl);
        vec3 spec = rectLightSpecular(N, V, fragPos, rl, albedo, metallic, roughness);
        vec3 F0r = mix(vec3(0.04), albedo, metallic);
        vec3 kD = (vec3(1.0) - F0r) * (1.0 - metallic);
        color += (kD * albedo / PI * diffuseGeo + spec) * radiance;
    }

    // Indirect term: IBL when the material enables it (matches Metal), else a
    // flat ambient floor. Screen-space AO darkens the indirect only.
    float screenAO = texture(texAO, gl_FragCoord.xy / max(screenSize, vec2(1.0))).r;
    if (mat.iblEnabled > 0.5) {
        color += calculateIBL(N, V, surf, occlusion) * screenAO;
    } else {
        color += albedo * 0.03 * occlusion * screenAO;
    }
    color += emissive;

    // Cascade debug: tint by shadow region (near map = green, cascades =
    // red/blue/yellow), matching the Metal PBR shader's visualization.
    if (debugVisualize != 0u) {
        vec3 tint;
        if (nearShadowEnd > 0.0 && viewDepth < nearShadowEnd) tint = vec3(0.2, 0.8, 0.2);
        else if (viewDepth <= cascadeSplits.y)                tint = vec3(0.8, 0.2, 0.2);
        else if (viewDepth <= cascadeSplits.z)                tint = vec3(0.2, 0.2, 0.8);
        else                                                  tint = vec3(0.8, 0.8, 0.2);
        vec3 Lviz = dirLightCount > 0u ? normalize(-dirLights[0].direction) : vec3(0.0, 1.0, 0.0);
        color = tint * max(dot(N, Lviz), 0.15) * sampleShadow(fragPos, N, Lviz, viewDepth);
    }

    // Output LINEAR HDR into the RGBA16F colorRT. Tone mapping (ACES) and the
    // sRGB encode happen in the PostProcess pass, so bloom and other effects
    // can operate on the HDR image beforehand.
    outColor = vec4(color, baseSample.a * mat.baseColorFactor.a * instanceColor.a);
}
