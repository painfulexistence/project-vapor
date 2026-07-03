#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"

// Debug screen-space resolve of the directional-light PSSM shadow.
// Mirrors the cascade-selection + PCF logic used inside the PBR fragment shader,
// but writes the resulting shadow factor into a camera-aligned screen-space
// texture so it can be inspected (and overlaps the scene) just like RT shadow.
//
// Output: R8Unorm, 0 = fully shadowed, 1 = fully lit.
kernel void computeMain(
    texture2d<float>                depthTexture  [[texture(0)]],
    depth2d_array<float, access::sample> pssmShadowMaps [[texture(1)]],
    texture2d<float, access::write> outputTexture [[texture(2)]],
    constant CameraData&            camera        [[buffer(0)]],
    constant PSSMData&              pssmData      [[buffer(1)]],
    constant float2&                screenSize    [[buffer(2)]],
    uint2 tid [[thread_position_in_grid]]
) {
    if (tid.x >= uint(screenSize.x) || tid.y >= uint(screenSize.y)) return;

    float depth = depthTexture.read(tid).r;
    if (depth >= 1.0) {
        outputTexture.write(float4(1.0), tid); // sky: lit
        return;
    }

    // Reconstruct world position (Metal LH ZO: flip Y for NDC)
    float2 uv = float2(tid) / screenSize;
    float2 ndcXY = float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float4 ndc = float4(ndcXY, depth, 1.0);
    float4 viewPos = camera.invProj * ndc;
    viewPos /= viewPos.w;
    float4 worldPos = camera.invView * viewPos;

    constexpr sampler shadowCmpSampler(
        address::clamp_to_edge,
        filter::linear,
        compare_func::less_equal
    );
    constexpr float PSSM_TEXEL = 1.0 / 4096.0;
    constexpr float PSSM_BIAS  = 0.002;

    // abs(): view matrix is RH (visible z is negative); splits are positive distances
    float viewDepth = abs((camera.view * worldPos).z);

    float shadowFactor = 1.0;
    if (viewDepth > pssmData.cascadeSplits.x) {
        int ci = 0;
        if (viewDepth > pssmData.cascadeSplits.z) ci = 2;
        else if (viewDepth > pssmData.cascadeSplits.y) ci = 1;

        float4 lsPos = pssmData.lightSpaceMatrices[ci] * worldPos;
        float3 proj  = lsPos.xyz / lsPos.w;
        float2 shadowUV = float2(proj.x * 0.5 + 0.5, 0.5 - proj.y * 0.5);
        float  refDepth = proj.z - PSSM_BIAS;

        float pcf = 0.0;
        for (int px = -1; px <= 1; px++) {
            for (int py = -1; py <= 1; py++) {
                pcf += pssmShadowMaps.sample_compare(
                    shadowCmpSampler,
                    shadowUV + float2(px, py) * PSSM_TEXEL,
                    ci, refDepth
                );
            }
        }
        shadowFactor = pcf / 9.0;
    }

    outputTexture.write(float4(shadowFactor, shadowFactor, shadowFactor, 1.0), tid);
}
