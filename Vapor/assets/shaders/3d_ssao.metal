#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal" // TODO: use more robust include path

// Screen-space AO (no ray tracing). Drop-in replacement for the raygen stage of
// the AO chain (3d_raytrace_ao.metal): same texture/buffer bindings minus the
// TLAS, same output contract (noisy single-channel AO, denoised by the
// temporal + à-trous passes). Resolution-agnostic: depth/normal may be at a
// higher resolution than the output (e.g. the half-res AO chain).
//
// Known limitations vs the RT version: no occlusion from off-screen or
// depth-occluded geometry, and silhouette halos are only suppressed (range
// check), not eliminated.

struct FrameData {
    uint frameNumber;
    float time;
    float deltaTime;
};

constant uint NUM_SAMPLES = 32;
constant float SSAO_RADIUS = 0.3; // meters; hemisphere radius and range-check scale
constant float SSAO_BIAS = 0.02;  // view-space depth bias

kernel void computeMain(
    texture2d<float> depthTexture [[texture(0)]],
    texture2d<float> normalTexture [[texture(1)]],
    texture2d<float, access::write> aoTexture [[texture(2)]],
    constant FrameData& frame [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = aoTexture.get_width();
    uint h = aoTexture.get_height();
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    uint2 fullDim = uint2(depthTexture.get_width(), depthTexture.get_height());
    uint2 fullMax = fullDim - 1;
    uint2 scale = max(fullDim / uint2(w, h), 1u);
    uint2 fullTid = min(tid * scale, fullMax);

    float depth = depthTexture.read(fullTid).r;
    // Sky / far plane: unoccluded, and its normal is zero (normalize would NaN)
    if (depth >= 0.999999) {
        aoTexture.write(float4(1.0), tid);
        return;
    }

    // Reconstruct world position — y-up NDC, same convention as the other kernels
    float2 uv = (float2(tid) + 0.5) / float2(w, h);
    uv.y = 1.0 - uv.y;
    float4 ndc = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 viewPos = camera.invProj * ndc;
    viewPos /= viewPos.w;
    float3 worldPos = (camera.invView * viewPos).xyz;
    float centerViewZ = viewPos.z;
    float3 worldNormal = normalize(normalTexture.read(fullTid).xyz);

    float occlusion = 0.0;
    uint count = 0;
    for (uint i = 0; i < NUM_SAMPLES; i++) {
        float2 xi = random(tid.x + tid.y * w + i * 7919u + frame.frameNumber * 26699u);
        float3 dir = sampleHemisphere(xi, worldNormal);
        // Distribute samples through the hemisphere volume, denser near the surface
        float t = (float(i) + 0.5) / float(NUM_SAMPLES);
        float3 samplePos = worldPos + worldNormal * 0.01 + dir * (SSAO_RADIUS * t * t);

        float4 clip = camera.proj * camera.view * float4(samplePos, 1.0);
        if (clip.w <= 0.0) continue; // behind the camera
        float3 sampleNdc = clip.xyz / clip.w;
        float2 sampleUVyUp = sampleNdc.xy * 0.5 + 0.5;
        if (any(sampleUVyUp != saturate(sampleUVyUp))) continue; // off-screen
        // Flip back from y-up NDC space to texture coordinates before reading
        float2 sampleTexUV = float2(sampleUVyUp.x, 1.0 - sampleUVyUp.y);
        uint2 sampleTid = min(uint2(sampleTexUV * float2(fullDim)), fullMax);
        float sceneDepth = depthTexture.read(sampleTid).r;

        // Compare linear view-space depths (NDC z is non-linear, so a fixed NDC
        // bias over-biases near and under-biases far). z is independent of xy
        // for a perspective projection, so (0,0,z,1) suffices.
        float4 sceneView = camera.invProj * float4(0.0, 0.0, sceneDepth, 1.0);
        float sceneViewZ = sceneView.z / sceneView.w;
        float4 sampleView = camera.invProj * float4(0.0, 0.0, sampleNdc.z, 1.0);
        float sampleViewZ = sampleView.z / sampleView.w;

        // RH view space looks down -z: the scene surface occludes the sample when
        // it is nearer to the camera (greater view z) than the sample point.
        if (sceneViewZ >= sampleViewZ + SSAO_BIAS) {
            // Range check: geometry far from the shading point shouldn't occlude
            // (classic silhouette-halo fix)
            float rangeCheck = smoothstep(0.0, 1.0, SSAO_RADIUS / max(abs(centerViewZ - sceneViewZ), 1e-4));
            occlusion += rangeCheck;
        }
        count++;
    }
    float result = count > 0 ? 1.0 - occlusion / float(count) : 1.0;
    aoTexture.write(float4(result, 0.0, 0.0, 0.0), tid);
}
