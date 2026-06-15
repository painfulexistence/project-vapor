#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"

// Depth-based screen-space motion vector pass.
// Reconstructs world position from the depth buffer, reprojects to previous
// frame clip space, and writes (currentUV - prevUV) in texture UV space.
// Sky pixels (depth == 1.0) get zero motion.
kernel void computeMain(
    texture2d<float>              depthTexture  [[texture(0)]],
    texture2d<float, access::write> motionTexture [[texture(1)]],
    constant CameraData&          camera        [[buffer(0)]],
    constant float4x4&            prevViewProj  [[buffer(1)]],
    constant float2&              screenSize    [[buffer(2)]],
    uint2 tid [[thread_position_in_grid]]
) {
    if (tid.x >= uint(screenSize.x) || tid.y >= uint(screenSize.y)) return;

    float depth = depthTexture.read(tid).r;

    // Sky — no geometry to reproject
    if (depth >= 1.0) {
        motionTexture.write(float4(0.0), tid);
        return;
    }

    // Texture UV (0,0 top-left)
    float2 uv = float2(tid) / screenSize;

    // Reconstruct world position (Metal LH ZO: flip Y for NDC)
    float2 ndcXY = float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float4 ndc   = float4(ndcXY, depth, 1.0);
    float4 viewPos = camera.invProj * ndc;
    viewPos /= viewPos.w;
    float3 worldPos = (camera.invView * viewPos).xyz;

    // Project to previous frame NDC
    float4 prevClip = prevViewProj * float4(worldPos, 1.0);
    float2 prevNDC  = prevClip.xy / prevClip.w;

    // Convert prev NDC to texture UV (flip Y back)
    float2 prevUV = float2(prevNDC.x * 0.5 + 0.5, 0.5 - prevNDC.y * 0.5);

    // Motion = current - prev (in texture UV space)
    float2 motion = uv - prevUV;
    motionTexture.write(float4(motion, 0.0, 0.0), tid);
}
