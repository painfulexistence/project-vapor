#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal" // TODO: use more robust include path

// Screen-Space Contact Shadows (SSCS).
//
// Fills the same near-field role as the ray-traced directional shadow, but works
// WITHOUT ray tracing — so the RHI/Vulkan path (which has no TLAS) still gets
// tight contact shadows where the PSSM cascades are too coarse. Output matches
// the RT shadow pass so the main lighting pass can consume it identically:
// R (0 = fully shadowed, 1 = fully lit), written into the near-field shadow RT.
//
// Method: from each visible pixel, march a short ray toward the light in VIEW
// space, projecting each step back to screen and comparing against the scene
// depth buffer. A sample that lands behind stored geometry — but not so far
// behind that it is a different surface (thickness gate) — is an occluder.

struct SSCSParams {
    float  rayLength;      // max marching distance in view-space units (contact scale)
    float  thickness;      // view-space depth window an occluder is allowed to span
    uint   stepCount;      // samples along the ray (8–24 typical)
    float  bias;           // view-space start offset to avoid self-occlusion
};

// View-space position from a depth sample. Depth is Metal ZO ([0,1]); the sign
// handling mirrors the RH/ZO convention used across the renderer (visible
// geometry sits at negative view z).
static float3 viewPosFromDepth(float2 uv, float depth, constant CameraData& camera) {
    // uv is top-left origin; NDC y is +1 at the top (Metal), so flip.
    float2 ndcXY = float2(uv.x, 1.0 - uv.y) * 2.0 - 1.0;
    float4 clip  = float4(ndcXY, depth, 1.0);
    float4 view  = camera.invProj * clip;
    return view.xyz / view.w;
}

kernel void computeMain(
    texture2d<float>                depthTexture  [[texture(0)]],
    texture2d<float, access::write> outputTexture [[texture(1)]],
    constant CameraData&            camera        [[buffer(0)]],
    const device DirLight*          directionalLights [[buffer(1)]],
    constant float2&                screenSize    [[buffer(2)]],
    constant SSCSParams&            params        [[buffer(3)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = outputTexture.get_width();
    uint h = outputTexture.get_height();
    if (tid.x >= w || tid.y >= h) return;

    // Resolution-agnostic: depth may be higher-res than the SSCS target.
    uint2 fullDim = uint2(depthTexture.get_width(), depthTexture.get_height());
    uint2 scale   = max(fullDim / uint2(w, h), 1u);
    uint2 fullTid = min(tid * scale, fullDim - 1);

    float depth = depthTexture.read(fullTid).r;
    if (depth >= 1.0) {                       // sky: fully lit
        outputTexture.write(float4(1.0), tid);
        return;
    }

    float2 uv = (float2(tid) + 0.5) / float2(w, h);
    float3 originVS = viewPosFromDepth(uv, depth, camera);

    // Light direction in VIEW space (directions ignore translation → mat3 * dir).
    // DirLight.direction is the direction the light travels; the ray toward the
    // light source is its negation.
    float3 lightDirWS = normalize(-directionalLights[0].direction);
    float3 lightDirVS = normalize((camera.view * float4(lightDirWS, 0.0)).xyz);

    float stepLen = params.rayLength / float(max(params.stepCount, 1u));
    float3 rayPos = originVS + lightDirVS * params.bias;

    float occlusion = 0.0;
    for (uint i = 0u; i < params.stepCount; ++i) {
        rayPos += lightDirVS * stepLen;

        // Project the marched view-space point back to screen UV.
        float4 clip = camera.proj * float4(rayPos, 1.0);
        if (clip.w <= 0.0) break;
        float3 ndc = clip.xyz / clip.w;
        float2 sampleUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 ||
            sampleUV.y < 0.0 || sampleUV.y > 1.0) break;

        uint2 sTid = min(uint2(sampleUV * float2(fullDim)), fullDim - 1);
        float sDepth = depthTexture.read(sTid).r;
        float3 sceneVS = viewPosFromDepth(sampleUV, sDepth, camera);

        // Both view-space z are negative (RH); "closer to camera" = larger z.
        // The scene surface occludes the ray if it sits in front of the ray
        // sample by more than nothing but less than `thickness` (so we skip
        // background surfaces far behind, which are not real contact occluders).
        // Require a margin (params.bias) so the ray's own origin surface — which
        // early steps project back onto — is not mistaken for an occluder. Without
        // this lower bound the whole frame self-occludes and goes black.
        float diff = sceneVS.z - rayPos.z;
        if (diff > params.bias && diff < params.thickness) {
            // Fade with marching distance so the contact term dies out smoothly
            // toward the ray's end instead of a hard cutoff.
            occlusion = 1.0 - float(i) / float(params.stepCount);
            break;
        }
    }

    outputTexture.write(float4(1.0 - occlusion), tid); // visibility: 1 lit, 0 shadowed
}
