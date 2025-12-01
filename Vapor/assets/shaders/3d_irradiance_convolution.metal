#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"

// Irradiance convolution shader
// Convolves the environment cubemap to create a diffuse irradiance map
// This integrates incoming light over the hemisphere for Lambertian diffuse

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
    texturecube<float, access::sample> environmentMap [[texture(0)]]
) {
    // The current direction is treated as the normal for diffuse lighting
    float3 normal = normalize(in.localPos);

    // Build TBN matrix for hemisphere sampling
    float3 up = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 right = normalize(cross(up, normal));
    up = normalize(cross(normal, right));

    constexpr sampler cubeSampler(filter::linear, mip_filter::linear);

    float3 irradiance = float3(0.0);
    int sampleCount = 0;

    // Sample hemisphere using spherical coordinates
    // This integrates: L_i * cos(theta) over hemisphere
    const float sampleDelta = 0.025;

    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
            // Spherical to Cartesian (tangent space)
            float3 tangentSample = float3(
                sin(theta) * cos(phi),
                sin(theta) * sin(phi),
                cos(theta)
            );

            // Transform to world space
            float3 sampleVec = tangentSample.x * right +
                              tangentSample.y * up +
                              tangentSample.z * normal;

            // Sample environment map and weight by cos(theta) * sin(theta)
            // cos(theta) = Lambert's law
            // sin(theta) = solid angle correction
            float3 envColor = environmentMap.sample(cubeSampler, sampleVec).rgb;
            irradiance += envColor * cos(theta) * sin(theta);
            sampleCount++;
        }
    }

    // Normalize by sample count and multiply by PI
    // (the PI factor comes from the integral normalization)
    irradiance = PI * irradiance / float(sampleCount);

    return float4(irradiance, 1.0);
}
