#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"

// Pre-filtered environment map shader for specular IBL
// Each mip level corresponds to a different roughness value
// Uses importance sampling with GGX distribution

struct IBLCaptureData {
    float4x4 viewProj;
    uint faceIndex;
    float roughness;
    float _pad[2];
};

// Full screen triangle vertices
constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct VertexOut {
    float4 position [[position]];
    float3 localPos;
};

// Convert cubemap face UV to world direction
float3 uvToDirection(float2 uv, uint face) {
    float2 st = uv * 2.0 - 1.0;

    float3 dir;
    switch (face) {
        case 0: dir = float3( 1.0, -st.y, -st.x); break; // +X
        case 1: dir = float3(-1.0, -st.y,  st.x); break; // -X
        case 2: dir = float3( st.x,  1.0,  st.y); break; // +Y
        case 3: dir = float3( st.x, -1.0, -st.y); break; // -Y
        case 4: dir = float3( st.x, -st.y,  1.0); break; // +Z
        case 5: dir = float3(-st.x, -st.y, -1.0); break; // -Z
    }

    return normalize(dir);
}

// Van Der Corpus sequence for low-discrepancy sampling
float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

// Hammersley sequence for quasi-random sampling
float2 hammersley(uint i, uint N) {
    return float2(float(i) / float(N), radicalInverseVdC(i));
}

// GGX importance sampling - generates a sample direction biased towards specular lobe
float3 importanceSampleGGX(float2 Xi, float3 N, float roughness) {
    float a = roughness * roughness;

    // Map random numbers to spherical coordinates
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    // Spherical to Cartesian (tangent space)
    float3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    // Build TBN matrix
    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    // Transform to world space
    float3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
    return normalize(sampleVec);
}

// GGX/Trowbridge-Reitz normal distribution function
float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;

    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return nom / denom;
}

vertex VertexOut vertexMain(
    uint vertexID [[vertex_id]],
    constant IBLCaptureData& capture [[buffer(0)]]
) {
    VertexOut out;
    out.position = float4(ndcVerts[vertexID], 0.0, 1.0);

    float2 uv = ndcVerts[vertexID] * 0.5 + 0.5;
    out.localPos = uvToDirection(uv, capture.faceIndex);

    return out;
}

fragment float4 fragmentMain(
    VertexOut in [[stage_in]],
    constant IBLCaptureData& capture [[buffer(0)]],
    texturecube<float, access::sample> environmentMap [[texture(0)]]
) {
    float3 N = normalize(in.localPos);
    // Assume view direction equals reflection direction (split sum approximation)
    float3 R = N;
    float3 V = R;

    float roughness = capture.roughness;

    constexpr sampler cubeSampler(filter::linear, mip_filter::linear);

    const uint SAMPLE_COUNT = 1024u;
    float3 prefilteredColor = float3(0.0);
    float totalWeight = 0.0;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
        float2 Xi = hammersley(i, SAMPLE_COUNT);
        float3 H = importanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            // Sample from environment map
            // Use mip level based on roughness and PDF to reduce aliasing
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float D = distributionGGX(NdotH, roughness);
            float pdf = D * NdotH / (4.0 * HdotV) + 0.0001;

            // Solid angle of current sample
            float resolution = 512.0; // Environment map resolution
            float saTexel = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);

            float mipLevel = roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);

            prefilteredColor += environmentMap.sample(cubeSampler, L, level(mipLevel)).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefilteredColor = prefilteredColor / totalWeight;

    return float4(prefilteredColor, 1.0);
}
