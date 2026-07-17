#ifndef COMMON_METAL
#define COMMON_METAL

#include <metal_stdlib>
using namespace metal;

constant float PI = 3.1415927;
constant float GAMMA = 2.2;
constant float INV_GAMMA = 1.0 / GAMMA;

constant uint MAX_LIGHTS_PER_CLUSTER = 256;
constant float MAX_CAMERA_DISTANCE = 1000.0f;

constant uint FRUSTUM_LEFT = 0;
constant uint FRUSTUM_RIGHT = 1;
constant uint FRUSTUM_BOTTOM = 2;
constant uint FRUSTUM_TOP = 3;
constant uint FRUSTUM_NEAR = 4;
constant uint FRUSTUM_FAR = 5;

struct VertexData {
    packed_float3 position;
    packed_float2 uv;
    packed_float3 normal;
    packed_float4 tangent;
};

struct CameraData {
    float4x4 proj;
    float4x4 view;
    float4x4 invProj;
    float4x4 invView;
    float near;
    float far;
    float3 position;
    float4 frustumPlanes[6];
};

struct InstanceData {
    float4x4 model;
    float4 color;
    uint vertexOffset;
    uint indexOffset;
    uint vertexCount;
    uint indexCount;
    uint materialID;
    uint primitiveMode;
    float3 AABBMin;
    float3 AABBMax;
    float4 boundingSphere; // x, y, z, radius
};

struct MaterialData {
    float4 baseColorFactor;
    float normalScale;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    float3 emissiveFactor;
    float emissiveStrength;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
    float prototypeUVMode; // 0 = Off, 1 = World Space, 2 = Object Space
    float uvScale;
    float iblEnabled; // 1.0 = use IBL, 0.0 = ambient approximation
    float transmission; // KHR_materials_transmission factor; IOR fixed 1.5
};

struct DirLight {
    float3 direction;
    float3 color;
    float intensity;
    // float _pad[3];
};

struct PointLight {
    float3 position;
    float3 color;
    float intensity;
    float radius;
    // float _pad[2];
};

// Cone spot light. direction points FROM the light; cosInner/cosOuter are the
// cosines of the inner (full-intensity) and outer (falloff-to-zero) half-angles.
// Must match Vapor::SpotLight (graphics.hpp, 64 bytes, vec3+scalar packing).
// float3 members occupy full 16-byte slots; scalars follow after the vectors
// (offsets 48/52/56/60) — mirrors the explicit pads on the C++ side.
struct SpotLight {
    float3 position;    // [0, 16)
    float3 direction;   // [16, 32) normalized, FROM the light
    float3 color;       // [32, 48)
    float  radius;      // 48 — range
    float  cosInner;    // 52
    float  cosOuter;    // 56
    float  intensity;   // 60
};

// Rectangular area light.  right and up are orthonormal axes of the light face;
// halfWidth/halfHeight give half-extents in those directions.
// packed_float3 is REQUIRED here: the C++ Vapor::RectLight tail-packs each
// scalar into the vec3's 4th float (offsets 0/12/16/28/32/44/48/60, 64 bytes).
// The previous plain-float3 declaration put every member on a 16-byte slot
// (position [0,16), halfWidth at 16 = C++ right.x, ... 128 bytes total) — every
// field except position read garbage. Latent for as long as no scene shipped
// rect lights; fatal the moment one does.
struct RectLight {
    packed_float3 position;   // 0
    float  halfWidth;         // 12
    packed_float3 right;      // 16 — normalized
    float  halfHeight;        // 28
    packed_float3 up;         // 32 — normalized
    float  intensity;         // 44
    packed_float3 color;      // 48
    uint   useVideoTexture;   // 60 — 0 = solid color, 1 = sample video texture
};

struct Cluster {
    float4 min;
    float4 max;
    uint lightCount;
    uint lightIndices[MAX_LIGHTS_PER_CLUSTER];
};

constant int PSSM_NUM_CASCADES = 3;

struct PSSMData {
    float4x4 lightSpaceMatrices[3];
    // view-space depths: x = RT shadow end, y = cascade1 end, z = cascade2 end, w = cascade3 end (far)
    float4 cascadeSplits;
    float blendRange;          // RT↔PSSM blend range
    float cascadeBlendRange;   // cascade↔cascade blend range (0 = hard transition)
    uint pcfSampleCount;       // 4, 8, 16, or 32
    uint debugVisualize;       // 0 = off, 1 = visualize cascades
};

// Poisson disk samples for high-quality PCF
constant float2 poissonDisk16[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
    float2(-0.094184101, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590),
    float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790)
};

constant float2 poissonDisk8[8] = {
    float2(-0.326212, -0.40581), float2(-0.840144, -0.07358),
    float2(-0.695914, 0.457137), float2(-0.203345, 0.620716),
    float2(0.96234, -0.194983), float2(0.473434, -0.480026),
    float2(0.519456, 0.767022), float2(0.185461, -0.893124)
};

constant float2 poissonDisk4[4] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
    float2(-0.094184101, -0.92938870), float2(0.34495938, 0.29387760)
};

float3x3 inverse(float3x3 const m) {
    float const A = m[1][1] * m[2][2] - m[2][1] * m[1][2];
    float const B = -(m[0][1] * m[2][2] - m[2][1] * m[0][2]);
    float const C = m[0][1] * m[1][2] - m[1][1] * m[0][2];
    float const D = -(m[1][0] * m[2][2] - m[2][0] * m[1][2]);
    float const E = m[0][0] * m[2][2] - m[2][0] * m[0][2];
    float const F = -(m[0][0] * m[1][2] - m[1][0] * m[0][2]);
    float const G = m[1][0] * m[2][1] - m[2][0] * m[1][1];
    float const H = -(m[0][0] * m[2][1] - m[2][0] * m[0][1]);
    float const I = m[0][0] * m[1][1] - m[1][0] * m[0][1];

    float const det = m[0][0] * A + m[1][0] * B + m[2][0] * C;
    if (abs(det) < 1e-6) {
        return float3x3(1.0);
    }
    float const invDet = 1.0f / det;

    return invDet * float3x3{
        float3(A, D, G),
        float3(B, E, H),
        float3(C, F, I)
    };
}

float2 random(uint seed) {
    uint s = seed * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return float2(float(w >> 22u) / 4294967295.0f,
                 float(w & 0x003FFFFFu) / 4194304.0f);
}

// uniform distribution on a unit sphere
float3 sampleSphere(float2 s) {
    float z = 2.0 * s.x - 1.0;
    float zz = sqrt(1.0 - z * z);
    float theta = 2.0 * PI * s.y;
    return float3(zz * cos(theta), zz * sin(theta), z);
}

// uniform distribution on a unit hemisphere
float3 sampleHemisphere(float2 s, float3 normal) {
    float3 dir = sampleSphere(s);
    return select(-dir, dir, dot(dir, normal) >= 0.0);
}

// cosine weighted distribution on a unit hemisphere
float3 sampleCosineWeightedHemisphere(float2 s, float3 normal) {
    float r = sqrt(s.x);
    float theta = 2.0 * PI * s.y;
    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(1.0 - s.x);
    float3 axis = select(float3(0, 0, 1), float3(1, 0, 0), abs(normal.z) > 0.999);
    float3 tangent = normalize(cross(axis, normal));
    float3 bitangent = cross(normal, tangent);
    return tangent * x + bitangent * y + normal * z;
}

// Octahedral unit-vector encoding — packs a normal into 2 floats so denoise
// chains can carry it in a texture channel instead of re-reading a normal RT.
float2 octEncode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = n.xy;
    if (n.z < 0.0) {
        e = (1.0 - abs(n.yx)) * select(float2(-1.0), float2(1.0), e >= 0.0);
    }
    return e;
}

float3 octDecode(float2 e) {
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) * select(float2(-1.0), float2(1.0), n.xy >= 0.0);
    }
    return normalize(n);
}

float3 linearToSRGB(float3 color) {
    // return pow(linear, float3(INV_GAMMA));
    return mix(
        color * 12.92,
        1.055 * pow(color, float3(1.0 / 2.4)) - 0.055,
        step(0.0031308, color)
    );
}

float3 srgbToLinear(float3 color) {
    // return pow(color, float3(GAMMA));
    return mix(
        color / 12.92,
        pow((color + 0.055) / 1.055, float3(2.4)),
        step(0.04045, color)
    );
}

#endif