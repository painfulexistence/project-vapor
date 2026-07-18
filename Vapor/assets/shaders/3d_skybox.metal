#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"   // CameraData

// Visible sky sampled from the captured environment cubemap (SkyType::HDRI).
// RHI-Metal twin of Skybox.frag. Fullscreen triangle at z = 1.0, depth-tested
// (LessOrEqual) to background pixels; reconstructs the world view direction the
// same way 3d_atmosphere.metal does, then samples the environment cubemap.

constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct SkyVertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex SkyVertexOut vertexMain(uint vertexID [[vertex_id]]) {
    SkyVertexOut out;
    out.position = float4(ndcVerts[vertexID], 1.0, 1.0);  // z = 1.0 far plane
    out.uv = ndcVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y;  // Metal Y-down UV
    return out;
}

fragment float4 fragmentMain(
    SkyVertexOut in [[stage_in]],
    constant CameraData& camera [[buffer(0)]],
    texturecube<float, access::sample> envCubemap [[texture(0)]]
) {
    constexpr sampler cubeSampler(filter::linear, mip_filter::linear);
    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Flip Y for Metal's coordinate system
    float4 clipPos = float4(ndc, 1.0, 1.0);
    float4 viewPos = camera.invProj * clipPos;
    viewPos /= viewPos.w;
    float3 dir = normalize((camera.invView * float4(viewPos.xyz, 0.0)).xyz);
    // Sample mip 0 explicitly: fullscreen cube sampling picks wrong (high) mips
    // at face boundaries via derivatives, tearing the sky.
    return float4(envCubemap.sample(cubeSampler, dir, level(0)).rgb, 1.0);
}
