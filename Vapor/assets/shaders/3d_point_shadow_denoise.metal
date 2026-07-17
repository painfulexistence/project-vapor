#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"
#include "Res/shaders/restir_shadow_common.metal"

constant float kShadowKernel1D[5] = { 1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0 };

// Edge-aware 5x5 cross-bilateral filter over the accumulated stochastic
// shadow factors (R point / G rect / B spot) — the "edge-aware spatial
// filtering" stage of the stochastic-RT skeleton (VISION.md), which the
// point-shadow chain never needed while its input was a stable binary
// visibility. Rect penumbras changed that: the accumulator's EMA carries a
// variance floor of alpha/(2-alpha) for a stationary noisy input (~±0.10 at a
// half-covered penumbra with 2 rays), which no amount of frames removes.
// Averaging ~25 geometry-compatible neighbors takes the residual below
// perception instead.
//
// Runs AFTER the temporal pass swaps its output into the history slot, and
// writes to the (now-scratch) denoised target that the PBR pass samples —
// history feedback stays UNfiltered, so the blur never compounds over frames
// (same layering as the AO chain: accumulate raw, display filtered).
//
// Edge stops: normal cone (pow32, like 3d_ao_denoise.metal) + distance from
// the center's tangent plane, so the filter never bleeds shadow across
// silhouettes or around corners. Geometry is reconstructed from depth via
// restirReconstructSurface — the same convention the ReSTIR kernels use.
kernel void computeMain(
    texture2d<float>              accumulated   [[texture(0)]],
    texture2d<float>              depthTexture  [[texture(1)]],
    texture2d<float>              normalTexture [[texture(2)]],
    texture2d<float, access::write> outShadow   [[texture(3)]],
    constant CameraData&          camera        [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = outShadow.get_width();
    uint h = outShadow.get_height();
    if (tid.x >= w || tid.y >= h) return;

    float3 center = accumulated.read(tid).rgb;
    float depth = depthTexture.read(tid).r;
    if (depth >= 1.0) {  // sky: nothing to filter
        outShadow.write(float4(center, 1.0), tid);
        return;
    }

    RestirSurface surf = restirReconstructSurface(tid, w, h, depth, camera);
    float3 centerN = normalize(normalTexture.read(tid).xyz);
    // Plane-distance scale: a few cm at 1 m, growing with distance so the
    // stop stays resolution- and range-independent.
    float planeScale = 0.02 * max(abs(surf.viewDepth), 0.5);

    float3 sum = float3(0.0);
    float weightSum = 0.0;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int2 tap = int2(tid) + int2(dx, dy);
            if (tap.x < 0 || tap.y < 0 || tap.x >= int(w) || tap.y >= int(h)) continue;
            uint2 tc = uint2(tap);

            float tapDepth = depthTexture.read(tc).r;
            if (tapDepth >= 1.0) continue;
            RestirSurface tapSurf = restirReconstructSurface(tc, w, h, tapDepth, camera);
            float3 tapN = normalize(normalTexture.read(tc).xyz);

            float wKernel = kShadowKernel1D[dx + 2] * kShadowKernel1D[dy + 2];
            float wNormal = pow(max(0.0, dot(centerN, tapN)), 32.0);
            float wPlane = exp(-abs(dot(tapSurf.worldPos - surf.worldPos, centerN)) / planeScale);

            float weight = wKernel * wNormal * wPlane;
            sum += accumulated.read(tc).rgb * weight;
            weightSum += weight;
        }
    }

    float3 filtered = weightSum > 1e-6 ? sum / weightSum : center;
    outShadow.write(float4(filtered, 1.0), tid);
}
