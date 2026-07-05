#version 450
// RHI renderer main-pass fragment shader (Vulkan backend).
// Simplified PBR: direct dir/point lighting, normal mapping, no clusters/IBL.
//
// Binding convention (see rhi_vulkan.cpp):
//   set 0 = vertex-stage buffers (visible here too; materials live in binding 1)
//   set 1 = fragment-stage buffers
//   set 2 = fragment textures
//   push constants: fragment bytes at offset 64+(binding%4)*16 in [64,128)

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
// Directional shadow light-space matrix (single cascade). Identity when no
// shadow pass ran, in which case sampling stays fully lit.
layout(std430, set = 1, binding = 2) readonly buffer ShadowBuf {
    mat4 shadowLightSpace;
};

layout(set = 2, binding = 0) uniform sampler2D albedoMap;
layout(set = 2, binding = 1) uniform sampler2D normalMap;
layout(set = 2, binding = 2) uniform sampler2D metallicMap;
layout(set = 2, binding = 3) uniform sampler2D roughnessMap;
layout(set = 2, binding = 4) uniform sampler2D occlusionMap;
layout(set = 2, binding = 5) uniform sampler2D emissiveMap;
layout(set = 2, binding = 6) uniform sampler2D shadowMap;  // directional depth map

// PCF directional shadow. Returns 1.0 = lit, 0.0 = fully shadowed.
float sampleShadow(vec3 worldPos, vec3 N, vec3 L) {
    vec4 lp = shadowLightSpace * vec4(worldPos, 1.0);
    vec3 proj = lp.xyz / lp.w;
    // Vulkan clip space: z in [0,1]; xy in [-1,1] -> map xy to [0,1] UV.
    vec2 uv = proj.xy * 0.5 + 0.5;
    float curDepth = proj.z;
    // Outside the shadow frustum (or behind) -> treat as lit.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || curDepth > 1.0) {
        return 1.0;
    }
    // Slope-scaled bias to reduce acne.
    float bias = max(0.0015 * (1.0 - dot(N, L)), 0.0004);
    vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float d = texture(shadowMap, uv + vec2(x, y) * texel).r;
            lit += (curDepth - bias) <= d ? 1.0 : 0.0;
        }
    }
    return lit / 9.0;
}

// RHI::setFragmentBytes(&lightCounts, 8, /*binding=*/7) -> offset 64+(7%4)*16 = 112
layout(push_constant) uniform PushConstants {
    layout(offset = 112) uvec2 lightCounts; // x = dir count, y = point count
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

    vec3 color = vec3(0.0);
    for (uint i = 0u; i < lightCounts.x; ++i) {
        DirLight l = dirLights[i];
        vec3 Ldir = normalize(-l.direction);
        vec3 contrib = shade(N, V, Ldir, l.color * l.intensity, albedo, metallic, roughness);
        // Only the first (sun) directional light casts the single-cascade shadow.
        if (i == 0u) contrib *= sampleShadow(fragPos, N, Ldir);
        color += contrib;
    }
    for (uint i = 0u; i < lightCounts.y; ++i) {
        PointLight l = pointLights[i];
        vec3 toLight = l.position - fragPos;
        float dist2 = max(dot(toLight, toLight), 1e-4);
        vec3 radiance = l.color * l.intensity / dist2;
        color += shade(N, V, normalize(toLight), radiance, albedo, metallic, roughness);
    }

    // Flat ambient so unlit scenes are never pure black
    color += albedo * 0.03 * occlusion;
    color += emissive;

    // Output LINEAR HDR into the RGBA16F colorRT. Tone mapping (ACES) and the
    // sRGB encode happen in the PostProcess pass, so bloom and other effects
    // can operate on the HDR image beforehand.
    outColor = vec4(color, baseSample.a * mat.baseColorFactor.a * instanceColor.a);
}
