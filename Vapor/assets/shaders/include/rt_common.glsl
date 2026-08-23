// Shared declarations for the Vulkan ray-query compute kernels (the GLSL twins
// of the 3d_raytrace_* / 3d_stochastic_* / gibs_* Metal kernels).
//
// Binding model (see rhi_vulkan.hpp "Descriptor Binding Model"):
//   set 0 = storage buffers   (setComputeBuffer, slots 0-15 on desktop)
//   set 1 = storage images    (setComputeTexture — kernel OUTPUTS)
//   set 2 = sampled textures  (setComputeSampledTexture — kernel INPUTS;
//                              depth can't be a storage image on Vulkan)
//   set 3 = acceleration structures (setAccelerationStructure)
//   set 4 = bindless material table (bindComputeTextureArgumentTable)
//   push constants: 16-byte slot at (binding % 8) * 16   (setComputeBytes)
// Slot NUMBERS match the Metal kernels' flat [[buffer/texture(N)]] indices, so
// the renderer pass code stays backend-agnostic.
//
// World reconstruction follows the proven Vulkan convention (SSAO.frag /
// VolumetricFog.frag): ndc = (u*2-1, 1-v*2, depth[ZO]).
#ifndef RT_COMMON_GLSL
#define RT_COMMON_GLSL

const float PI = 3.1415927;

// ---------------------------------------------------------------------------
// GPU struct twins (layouts asserted against C++ in graphics_gpu_structs.hpp)
// ---------------------------------------------------------------------------

// Must match Vapor::CameraRenderData.
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

// Must match DirectionalLightData / PointLightData (C++, stride 48 each).
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

// Must match Vapor::SpotLight (std430, 64 bytes).
struct SpotLight {
    vec3 position;  float _pad0;
    vec3 direction; float _pad1;
    vec3 color;     float _pad2;
    float radius;   float cosInner; float cosOuter; float intensity;
};

// Must match Vapor::RectLight (std430 packs the scalars into the vec3 tails).
struct RectLight {
    vec3 position;  float halfWidth;
    vec3 rright;    float halfHeight;
    vec3 up;        float intensity;
    vec3 color;     uint useVideoTexture;
};

// Must match the Cluster layout in LightCull.comp / RHIMain.frag.
const uint MAX_LIGHTS_PER_TILE = 256u;
const uint MAX_SPOTS_PER_CLUSTER = 64u;
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

// Must match Vapor::InstanceData. rtVertexOffset/rtIndexOffset are the
// always-valid merged-buffer offsets for RT hit shading (the Metal twin
// documents how they occupy the C++ struct's _pad1 slot).
struct InstanceData {
    mat4 model;
    vec4 color;
    uint vertexOffset;
    uint indexOffset;
    uint vertexCount;
    uint indexCount;
    uint materialID;
    uint primitiveMode;
    uint rtVertexOffset;
    uint rtIndexOffset;
    vec3 aabbMin;
    float _pad2;
    vec3 aabbMax;
    float _pad3;
    vec4 boundingSphere;
};

// Must match Vapor::MaterialData (std430 stride 112).
struct MaterialData {
    vec4 baseColorFactor;
    float normalScale;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    vec3 emissiveFactor;
    float alphaCutoff;
    float emissiveStrength;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
    float prototypeUVMode;
    float uvScale;
    float iblEnabled;
    float transmission;
};

// ---------------------------------------------------------------------------
// RNG (transliterated from 3d_common.metal — identical sequences)
// ---------------------------------------------------------------------------

vec2 rand2(uint seed) {
    uint s = seed * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return vec2(float(w >> 22u) / 4294967295.0,
                float(w & 0x003FFFFFu) / 4194304.0);
}

// Stateful variant: same PCG hash, advances `state`.
float randomNext(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float(word >> 8u) * (1.0 / 16777216.0); // [0, 1)
}

// Cosine-weighted hemisphere sample around `normal` (matches 3d_common.metal).
vec3 sampleCosineWeightedHemisphere(vec2 s, vec3 normal) {
    float r = sqrt(s.x);
    float theta = 2.0 * PI * s.y;
    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(1.0 - s.x);
    vec3 axis = abs(normal.z) > 0.999 ? vec3(1, 0, 0) : vec3(0, 0, 1);
    vec3 tangent = normalize(cross(axis, normal));
    vec3 bitangent = cross(normal, tangent);
    return tangent * x + bitangent * y + normal * z;
}

// Octahedral unit-vector encode/decode — exact twins of 3d_common.metal
// (e stays in [-1,1]; select(a,b,cond) == cond ? b : a).
vec2 signNonNeg2(vec2 v) {
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 e = n.xy;
    if (n.z < 0.0) {
        e = (1.0 - abs(n.yx)) * signNonNeg2(e);
    }
    return e;
}

vec3 octDecode(vec2 e) {
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * signNonNeg2(n.xy);
    }
    return normalize(n);
}

const vec2 poissonDisk8[8] = vec2[8](
    vec2(-0.326212, -0.40581), vec2(-0.840144, -0.07358),
    vec2(-0.695914, 0.457137), vec2(-0.203345, 0.620716),
    vec2(0.96234, -0.194983), vec2(0.473434, -0.480026),
    vec2(0.519456, 0.767022), vec2(0.185461, -0.893124)
);

// MSL isfinite() equivalent.
bool isFiniteF(float x) { return !isnan(x) && !isinf(x); }

// ---------------------------------------------------------------------------
// World reconstruction (Vulkan y-up NDC + ZO depth)
// ---------------------------------------------------------------------------

// uv in [0,1] (v = 0 at the TOP texture row), ZO depth from the depth RT.
vec3 worldPosFromDepth(vec2 uv, float depth, mat4 invProj, mat4 invView) {
    vec4 ndc = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    vec4 viewPos = invProj * ndc;
    viewPos /= viewPos.w;
    return (invView * viewPos).xyz;
}

#endif // RT_COMMON_GLSL
