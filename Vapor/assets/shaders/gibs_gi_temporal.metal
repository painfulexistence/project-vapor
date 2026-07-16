#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"

// SVGF-lite step 1 for GIBS GI: temporal reprojection over the screen-space
// gather output. A straight RGB port of 3d_ao_temporal.metal (the proven AO
// skeleton): reproject via the velocity buffer, disocclusion-check against the
// view-space depth stored in history alpha, and EMA-blend. Unlike the AO
// history, normals are NOT packed here — the denoise pass reads the full-res
// normal RT directly for its edge stops.
//
// History texture is RGBA16F: RGB = accumulated GI, A = view-space depth.
kernel void computeMain(
    texture2d<float> giRaw [[texture(0)]],
    texture2d<float, access::sample> historyIn [[texture(1)]],
    texture2d<float, access::write> historyOut [[texture(2)]],
    texture2d<float> velocityTexture [[texture(3)]],
    texture2d<float> depthTexture [[texture(4)]],
    constant CameraData& camera [[buffer(0)]],
    constant float4x4& prevView [[buffer(1)]],
    constant uint& historyValid [[buffer(2)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = historyOut.get_width();
    uint h = historyOut.get_height();
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    // Resolution-agnostic: depth/velocity are full-res, the GI chain runs at
    // the GIBS resolution scale.
    uint2 fullDim = uint2(depthTexture.get_width(), depthTexture.get_height());
    uint2 scale = max(fullDim / uint2(w, h), 1u);
    uint2 fullTid = min(tid * scale, fullDim - 1);

    float3 gi = giRaw.read(tid).rgb;
    float depth = depthTexture.read(fullTid).r;

    // Sky: no GI, park the history at far depth.
    if (depth >= 0.999999) {
        historyOut.write(float4(0.0, 0.0, 0.0, -camera.far), tid);
        return;
    }

    // Reconstruct world position (same convention as the raytrace kernels).
    float2 texUV = (float2(tid) + 0.5) / float2(w, h);
    float2 uvYUp = float2(texUV.x, 1.0 - texUV.y);
    float4 ndc = float4(uvYUp * 2.0 - 1.0, depth, 1.0);
    float4 viewPos = camera.invProj * ndc;
    viewPos /= viewPos.w;
    float3 worldPos = (camera.invView * viewPos).xyz;
    float viewZ = viewPos.z;

    float3 blended = gi;
    if (historyValid != 0) {
        float2 vel = velocityTexture.read(fullTid).rg;      // y-up NDC*0.5 units
        float2 prevUVyUp = uvYUp - vel;
        float2 prevTexUV = float2(prevUVyUp.x, 1.0 - prevUVyUp.y);

        bool inBounds = all(prevTexUV == saturate(prevTexUV));
        if (inBounds) {
            constexpr sampler s(filter::linear, address::clamp_to_edge);
            float4 hist = historyIn.sample(s, prevTexUV);

            // Where should this surface have been in the previous view?
            float expectedPrevZ = (prevView * float4(worldPos, 1.0)).z;
            bool depthMatches = abs(hist.a - expectedPrevZ) < 0.05 * max(abs(expectedPrevZ), 1.0);
            if (depthMatches) {
                blended = mix(hist.rgb, gi, 0.1);
            }
        }
    }
    historyOut.write(float4(blended, viewZ), tid);
}
