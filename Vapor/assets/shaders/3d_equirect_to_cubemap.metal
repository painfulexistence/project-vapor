#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"

// Converts an equirectangular HDR texture into a single cubemap face.
// Vertex stage emits a fullscreen triangle; the fragment converts each
// pixel's cubemap-face direction back to equirectangular (u, v) coords.

struct IBLCaptureData {
    float4x4 viewProj;
    uint     faceIndex;
    float    roughness;
    float    _pad[2];
};

constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct VertexOut {
    float4 position [[position]];
    float3 localPos;
};

// Map (face, uv) → normalised world direction (same convention as 3d_sky_capture.metal)
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
        default: dir = float3(0.0, 0.0, 1.0);
    }
    // Return the UN-normalized direction: it is affine in uv, so the rasterizer
    // interpolates it exactly across the fullscreen triangle. Normalizing here
    // (per vertex) then interpolating would lerp normalized corners and bend the
    // per-pixel direction (~24deg off at a face centre), distorting the capture.
    // The fragment stage re-normalizes localPos.
    return dir;
}

vertex VertexOut vertexMain(
    uint vertexID [[vertex_id]],
    constant IBLCaptureData& capture [[buffer(0)]]
) {
    VertexOut out;
    out.position = float4(ndcVerts[vertexID], 0.0, 1.0);
    float2 uv    = ndcVerts[vertexID] * 0.5 + 0.5;
    out.localPos = uvToDirection(uv, capture.faceIndex);
    return out;
}

fragment float4 fragmentMain(
    VertexOut in [[stage_in]],
    texture2d<float, access::sample> equirectTexture [[texture(0)]]
) {
    constexpr sampler s(filter::linear, address::repeat);

    float3 dir = normalize(in.localPos);

    // Spherical → equirectangular UV
    float phi   = atan2(dir.z, dir.x);           // [-π, π]
    float theta = asin(clamp(dir.y, -1.0f, 1.0f)); // [-π/2, π/2]

    float u = phi   / (2.0 * PI) + 0.5;
    float v = 1.0 - (theta / PI + 0.5);           // flip V: image top = sky

    return equirectTexture.sample(s, float2(u, v));
}
