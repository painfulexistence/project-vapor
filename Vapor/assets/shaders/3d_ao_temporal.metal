#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal" // TODO: use more robust include path

// Temporal accumulation for RT AO (ADR-008 step 2).
// History texture is RGBA16F: R = accumulated AO, G = view-space depth,
// BA = octahedral-encoded world normal (carried for the denoise passes, which
// then never touch the full-res normal RT). Disocclusion check: reproject the
// current world position into the previous view and compare against the
// stored depth.
kernel void computeMain(
    texture2d<float> aoRaw [[texture(0)]],
    texture2d<float, access::sample> historyIn [[texture(1)]],
    texture2d<float, access::write> historyOut [[texture(2)]],
    texture2d<float> velocityTexture [[texture(3)]],
    texture2d<float> depthTexture [[texture(4)]],
    texture2d<float> normalTexture [[texture(5)]],
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

    // AO chain is half-res; depth and velocity are full-res
    uint2 fullTid = min(tid * 2, uint2(depthTexture.get_width() - 1, depthTexture.get_height() - 1));

    float ao = aoRaw.read(tid).r;
    float depth = depthTexture.read(fullTid).r;

    // Sky: unoccluded, park the history at far depth
    if (depth >= 0.999999) {
        historyOut.write(float4(1.0, -camera.far, 0.0, 0.0), tid);
        return;
    }

    float3 worldNormal = normalize(normalTexture.read(fullTid).xyz);

    // Reconstruct world position (same convention as the raytrace kernels)
    float2 texUV = (float2(tid) + 0.5) / float2(w, h);
    float2 uvYUp = float2(texUV.x, 1.0 - texUV.y);
    float4 ndc = float4(uvYUp * 2.0 - 1.0, depth, 1.0);
    float4 viewPos = camera.invProj * ndc;
    viewPos /= viewPos.w;
    float3 worldPos = (camera.invView * viewPos).xyz;
    float viewZ = viewPos.z;

    float blended = ao;
    if (historyValid != 0) {
        float2 vel = velocityTexture.read(fullTid).rg;      // y-up NDC*0.5 units
        float2 prevUVyUp = uvYUp - vel;
        float2 prevTexUV = float2(prevUVyUp.x, 1.0 - prevUVyUp.y);

        bool inBounds = all(prevTexUV == saturate(prevTexUV));
        if (inBounds) {
            constexpr sampler s(filter::linear, address::clamp_to_edge);
            float2 hist = historyIn.sample(s, prevTexUV).rg;

            // Where should this surface have been in the previous view?
            float expectedPrevZ = (prevView * float4(worldPos, 1.0)).z;
            bool depthMatches = abs(hist.g - expectedPrevZ) < 0.05 * max(abs(expectedPrevZ), 1.0);
            if (depthMatches) {
                blended = mix(hist.r, ao, 0.1);
            }
        }
    }
    historyOut.write(float4(blended, viewZ, octEncode(worldNormal)), tid);
}
