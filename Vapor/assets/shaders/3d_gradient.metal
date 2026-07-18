#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"   // CameraData

// Cheap gradient sky (SkyType::Gradient). RHI-Metal twin of Gradient.frag.
// Fullscreen triangle at z = 1.0, depth-tested (LessOrEqual) to background
// pixels; reconstructs the world view direction the same way 3d_skybox.metal
// does, then blends a zenith/horizon/ground vertical gradient.

constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct SkyVertexOut {
    float4 position [[position]];
    float2 uv;
};

// Zenith/horizon/ground colors (matches Vapor::GradientRenderData / Gradient.frag).
struct GradientData {
    float4 zenith;
    float4 horizon;
    float4 ground;
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
    constant GradientData& grad [[buffer(1)]]
) {
    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Flip Y for Metal's coordinate system
    float4 clipPos = float4(ndc, 1.0, 1.0);
    float4 viewPos = camera.invProj * clipPos;
    viewPos /= viewPos.w;
    float3 dir = normalize((camera.invView * float4(viewPos.xyz, 0.0)).xyz);

    float3 sky;
    if (dir.y >= 0.0) {
        sky = mix(grad.horizon.rgb, grad.zenith.rgb, pow(clamp(dir.y, 0.0, 1.0), 0.5));
    } else {
        sky = mix(grad.horizon.rgb, grad.ground.rgb, pow(clamp(-dir.y, 0.0, 1.0), 0.5));
    }
    return float4(sky, 1.0);
}
