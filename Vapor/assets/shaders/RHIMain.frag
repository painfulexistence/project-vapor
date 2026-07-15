#version 450
// RHI renderer main-pass fragment shader (Vulkan backend).
// Simplified PBR: direct dir/point lighting, normal mapping, no clusters/IBL.
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
// identical algorithm on the same GPU). Safe here: the shader writes only
// color, never discards, never writes gl_FragDepth.
layout(early_fragment_tests) in;

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 worldNormal;
layout(location = 3) in vec4 worldTangent;
layout(location = 4) flat in uint fragMaterialID;
layout(location = 5) in vec4 instanceColor;

layout(location = 0) out vec4 outColor;

// Must match Vapor::MaterialData (C++, std430 stride = 96)
struct MaterialData {
    vec4 baseColorFactor;
    float normalScale;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    vec3 emissiveFactor;
    float _pad1;
    float emissiveStrength;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
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
struct Cluster {
    vec4 mn;
    vec4 mx;
    uint lightCount;
    uint lightIndices[MAX_LIGHTS_PER_TILE];
};
layout(std430, set = 1, binding = 4) readonly buffer ClusterBuf { Cluster tiles[]; };
layout(std430, set = 1, binding = 5) readonly buffer LightCullBuf {
    vec2 cullScreenSize;
    vec2 _cpad1;
    uvec3 cullGridSize;
    uint cullLightCount;
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

layout(set = 2, binding = 0) uniform sampler2D albedoMap;
layout(set = 2, binding = 1) uniform sampler2D normalMap;
layout(set = 2, binding = 2) uniform sampler2D metallicMap;
layout(set = 2, binding = 3) uniform sampler2D roughnessMap;
layout(set = 2, binding = 4) uniform sampler2D occlusionMap;
layout(set = 2, binding = 5) uniform sampler2D emissiveMap;
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

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 shade(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float roughness) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(NdotV, NdotL, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / PI + specular) * radiance * NdotL;
}

void main() {
    MaterialData mat = materials[fragMaterialID];

    vec4 baseSample = texture(albedoMap, fragUV);
    vec3 albedo = baseSample.rgb * mat.baseColorFactor.rgb * instanceColor.rgb;
    float metallic = texture(metallicMap, fragUV).b * mat.metallicFactor;
    float roughness = clamp(texture(roughnessMap, fragUV).g * mat.roughnessFactor, 0.04, 1.0);
    float occlusion = mix(1.0, texture(occlusionMap, fragUV).r, mat.occlusionStrength);
    vec3 emissive = texture(emissiveMap, fragUV).rgb * mat.emissiveFactor * mat.emissiveStrength;

    // Normal mapping (fall back to the geometric normal when tangent is degenerate)
    vec3 N = normalize(worldNormal);
    vec3 T = worldTangent.xyz;
    if (dot(T, T) > 1e-8) {
        T = normalize(T - dot(T, N) * N);
        vec3 B = cross(N, T) * worldTangent.w;
        vec3 nSample = texture(normalMap, fragUV).xyz * 2.0 - 1.0;
        nSample.xy *= mat.normalScale;
        N = normalize(mat3(T, B, N) * nSample);
    }

    vec3 V = normalize(cam.position - fragPos);

    // View-space forward distance selects the shadow cascade (RH: forward = -z).
    float viewDepth = -(cam.view * vec4(fragPos, 1.0)).z;

    vec3 color = vec3(0.0);
    for (uint i = 0u; i < dirLightCount; ++i) {
        DirLight l = dirLights[i];
        vec3 Ldir = normalize(-l.direction);
        vec3 contrib = shade(N, V, Ldir, l.color * l.intensity, albedo, metallic, roughness);
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
    if ((mainDebugFlags & 1u) == 0u) {
        vec4 clip = cam.proj * cam.view * vec4(fragPos, 1.0);
        vec2 suv = clamp((clip.xy / max(clip.w, 1e-4)) * 0.5 + 0.5, vec2(0.0), vec2(0.9999));
        uvec2 tile = uvec2(suv * vec2(cullGridSize.xy));
        uint tileIndex = tile.x + tile.y * cullGridSize.x;
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
            color += shade(N, V, normalize(toLight), radiance, albedo, metallic, roughness);
        }
    }

    // Flat ambient so unlit scenes are never pure black. Screen-space AO
    // darkens this indirect term only (never the direct lights above).
    float screenAO = texture(texAO, gl_FragCoord.xy / max(screenSize, vec2(1.0))).r;
    color += albedo * 0.03 * occlusion * screenAO;
    color += emissive;

    // Output LINEAR HDR into the RGBA16F colorRT. Tone mapping (ACES) and the
    // sRGB encode happen in the PostProcess pass, so bloom and other effects
    // can operate on the HDR image beforehand.
    outColor = vec4(color, baseSample.a * mat.baseColorFactor.a * instanceColor.a);
}
