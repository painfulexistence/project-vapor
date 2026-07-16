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
    float shadowBlendRange;
    float nearShadowEnd;        // view depth the independent near map covers; 0 = off
    vec2 _pssmPad;
    mat4 nearLightMatrix;       // near-field map (own texture, nearShadowTex)
};

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
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float d = texture(shadowMap, vec3(uv + vec2(x, y) * texel, float(ci))).r;
            lit += (curDepth - bias) <= d ? 1.0 : 0.0;
        }
    }
    return lit / 9.0;
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
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float d = texture(nearShadowTex, uv + vec2(x, y) * texel).r;
            lit += (curDepth - bias) <= d ? 1.0 : 0.0;
        }
    }
    return lit / 9.0;
}

// Select the near-field map or a cascade by the fragment's view-space depth,
// PCF-sample it, and cross-fade across boundaries so transitions don't seam.
// Returns 1.0 = lit, 0.0 = fully shadowed. Boundaries (view depth):
//   nearShadowEnd = near map -> cascade 0,  cascadeSplits.y = c0 -> c1,
//   cascadeSplits.z = c1 -> c2. Blend band width = shadowBlendRange.
float sampleShadow(vec3 worldPos, vec3 N, vec3 L, float viewDepth) {
    float b = max(0.0015 * (1.0 - dot(N, L)), 0.0004);
    float blend = max(shadowBlendRange, 1e-4);

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
    vec3 B;
    if (dot(T, T) > 1e-8) {
        T = normalize(T - dot(T, N) * N);
        B = cross(N, T) * worldTangent.w;
        vec3 nSample = texture(normalMap, fragUV).xyz * 2.0 - 1.0;
        nSample.xy *= mat.normalScale;
        N = normalize(mat3(T, B, N) * nSample);
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
    Surface surf;
    surf.color = albedo;
    surf.roughness = roughness;
    surf.metallic = metallic;
    surf.subsurface = mat.subsurface;
    surf.specular = mat.specular;
    surf.specularTint = mat.specularTint;
    surf.anisotropic = mat.anisotropic;
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
            color += disneyBRDF(N, T, B, normalize(toLight), V, surf) * radiance;
        }
    }

    // Spot lights (loop-all; typical scenes carry a handful). Same falloff as
    // point lights, windowed by a squared smooth cone factor between the inner
    // and outer half-angle cosines. Unshadowed on Vulkan until RT lands here.
    for (uint sIdx = 0u; sIdx < spotRectCounts.x; ++sIdx) {
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
    for (uint rIdx = 0u; rIdx < spotRectCounts.y; ++rIdx) {
        RectLight rl = rectLights[rIdx];
        vec3 radiance = rl.color * rl.intensity;
        float diffuseGeo = rectLightDiffuse(N, fragPos, rl);
        vec3 spec = rectLightSpecular(N, V, fragPos, rl, albedo, metallic, roughness);
        vec3 F0r = mix(vec3(0.04), albedo, metallic);
        vec3 kD = (vec3(1.0) - F0r) * (1.0 - metallic);
        color += (kD * albedo / PI * diffuseGeo + spec) * radiance;
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
