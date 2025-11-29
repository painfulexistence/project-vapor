#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"

// BRDF Integration LUT shader
// Pre-computes the split-sum BRDF integration for IBL specular
// Output: RG = scale and bias for F0, stored in a 2D texture
// X axis = NdotV (cos theta), Y axis = roughness

// Full screen triangle vertices
constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// Van Der Corpus sequence
float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

// Hammersley sequence
float2 hammersley(uint i, uint N) {
    return float2(float(i) / float(N), radicalInverseVdC(i));
}

// GGX importance sampling
float3 importanceSampleGGX(float2 Xi, float3 N, float roughness) {
    float a = roughness * roughness;

    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    float3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);

    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

// Geometry function (Schlick-GGX)
float geometrySchlickGGX(float NdotV, float roughness) {
    // Note: using k = (roughness^2) / 2 for IBL
    float a = roughness;
    float k = (a * a) / 2.0;

    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / denom;
}

// Smith's method for geometry
float geometrySmith(float3 N, float3 V, float3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = geometrySchlickGGX(NdotV, roughness);
    float ggx1 = geometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

// Integrate BRDF for given NdotV and roughness
float2 integrateBRDF(float NdotV, float roughness) {
    float3 V;
    V.x = sqrt(1.0 - NdotV * NdotV); // sin(theta)
    V.y = 0.0;
    V.z = NdotV; // cos(theta)

    float A = 0.0; // Scale
    float B = 0.0; // Bias

    float3 N = float3(0.0, 0.0, 1.0);

    const uint SAMPLE_COUNT = 1024u;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
        float2 Xi = hammersley(i, SAMPLE_COUNT);
        float3 H = importanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);

        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);

        if (NdotL > 0.0) {
            float G = geometrySmith(N, V, L, roughness);
            float G_Vis = (G * VdotH) / (NdotH * NdotV);
            float Fc = pow(1.0 - VdotH, 5.0);

            A += (1.0 - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    A /= float(SAMPLE_COUNT);
    B /= float(SAMPLE_COUNT);

    return float2(A, B);
}

vertex VertexOut vertexMain(uint vertexID [[vertex_id]]) {
    VertexOut out;
    out.position = float4(ndcVerts[vertexID], 0.0, 1.0);
    out.uv = ndcVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y; // Flip Y
    return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]]) {
    // UV.x = NdotV, UV.y = roughness
    float NdotV = in.uv.x;
    float roughness = in.uv.y;

    // Prevent division by zero
    NdotV = max(NdotV, 0.001);
    roughness = max(roughness, 0.001);

    float2 brdf = integrateBRDF(NdotV, roughness);

    return float4(brdf, 0.0, 1.0);
}
