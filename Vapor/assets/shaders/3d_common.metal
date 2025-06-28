#include <metal_stdlib>
using namespace metal;

constant float PI = 3.1415927;
constant float GAMMA = 2.2;
constant float INV_GAMMA = 1.0 / GAMMA;

struct VertexData {
    packed_float3 position;
    packed_float2 uv;
    packed_float3 normal;
    float4 tangent;
    // packed_float3 bitangent;
};

struct CameraData {
    float4x4 proj;
    float4x4 view;
    float4x4 invProj;
    float4x4 invView;
    float near;
    float far;
};

struct InstanceData {
    float4x4 model;
    float4 color;
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
    float const invDet = 1.f / det;
    return invDet * float3x3{
        float3(A, B, C),
        float3(D, E, F),
        float3(G, H, I)
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