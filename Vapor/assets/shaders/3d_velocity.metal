#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal" // TODO: use more robust include path

// Camera-motion velocity from the depth buffer.
// velocity = (currNDC.xy - prevNDC.xy) * 0.5, i.e. UV-scale units in y-up NDC
// space. Consumers reproject with prevUV = yUpUV - velocity.
// TODO: per-instance object motion needs prevModel tracking on the ECS side;
// until then, moving objects rely on the temporal passes' disocclusion checks.
kernel void computeMain(
    texture2d<float> depthTexture [[texture(0)]],
    texture2d<float, access::write> velocityTexture [[texture(1)]],
    constant CameraData& camera [[buffer(0)]],
    constant float4x4& prevViewProj [[buffer(1)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = velocityTexture.get_width();
    uint h = velocityTexture.get_height();
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    float2 uv = (float2(tid) + 0.5) / float2(w, h);
    uv.y = 1.0 - uv.y; // y-up UV, matching the raytrace kernels' convention
    float depth = depthTexture.read(tid).r;
    float4 ndc = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 viewPos = camera.invProj * ndc;
    viewPos /= viewPos.w;
    float3 worldPos = (camera.invView * viewPos).xyz;

    float2 velocity = float2(0.0);
    float4 prevClip = prevViewProj * float4(worldPos, 1.0);
    if (prevClip.w > 0.0) {
        float2 prevNDC = prevClip.xy / prevClip.w;
        velocity = (ndc.xy - prevNDC) * 0.5;
    }
    velocityTexture.write(float4(velocity, 0.0, 0.0), tid);
}
