#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"
#include "Res/shaders/restir_shadow_common.metal"

constant float kAtrous3[3] = { 0.25, 0.5, 0.25 };

// Edge-aware a-trous denoise over the accumulated stochastic shadow factors
// (R point / G rect / B spot) — the spatial-filtering stage of the
// stochastic-RT skeleton (VISION.md), run iteratively at increasing stride
// (SVGF-lite, matching 3d_ao_denoise / gibs_gi_denoise). A 3x3 kernel dilated
// by `stride` reaches a wide radius over a couple of passes at FEWER taps than
// one wide single pass (2x 3x3 = 18 vs the old 5x5's 25), so it is both wider
// and cheaper. Each pass reads the previous pass's output (ping-pong); the
// UNfiltered accumulator history feeds the temporal loop, so the blur never
// compounds across frames (accumulate raw, display filtered — the AO layering).
//
// Edge stops: normal cone (pow32) + distance from the centre's tangent plane,
// so shadow never bleeds across silhouettes or around corners even as the
// stride widens. Geometry is reconstructed from depth via
// restirReconstructSurface — the same convention the ReSTIR kernels use.
kernel void computeMain(
    texture2d<float>              src           [[texture(0)]],
    texture2d<float>              depthTexture  [[texture(1)]],
    texture2d<float>              normalTexture [[texture(2)]],
    texture2d<float, access::write> dst         [[texture(3)]],
    constant CameraData&          camera        [[buffer(0)]],
    constant uint&                stride        [[buffer(1)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = dst.get_width();
    uint h = dst.get_height();
    if (tid.x >= w || tid.y >= h) return;

    float3 center = src.read(tid).rgb;
    float depth = depthTexture.read(tid).r;
    if (depth >= 1.0) {  // sky: nothing to filter
        dst.write(float4(center, 1.0), tid);
        return;
    }

    RestirSurface surf = restirReconstructSurface(tid, w, h, depth, camera);
    float3 centerN = normalize(normalTexture.read(tid).xyz);
    // Plane-distance scale: a few cm at 1 m, growing with distance so the stop
    // stays resolution- and range-independent.
    float planeScale = 0.02 * max(abs(surf.viewDepth), 0.5);

    float3 sum = float3(0.0);
    float weightSum = 0.0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int2 tap = int2(tid) + int2(dx, dy) * int(stride);
            if (tap.x < 0 || tap.y < 0 || tap.x >= int(w) || tap.y >= int(h)) continue;
            uint2 tc = uint2(tap);

            float tapDepth = depthTexture.read(tc).r;
            if (tapDepth >= 1.0) continue;
            RestirSurface tapSurf = restirReconstructSurface(tc, w, h, tapDepth, camera);
            float3 tapN = normalize(normalTexture.read(tc).xyz);

            float wKernel = kAtrous3[dx + 1] * kAtrous3[dy + 1];
            float wNormal = pow(max(0.0, dot(centerN, tapN)), 32.0);
            float wPlane = exp(-abs(dot(tapSurf.worldPos - surf.worldPos, centerN)) / planeScale);

            float weight = wKernel * wNormal * wPlane;
            sum += src.read(tc).rgb * weight;
            weightSum += weight;
        }
    }

    float3 filtered = weightSum > 1e-6 ? sum / weightSum : center;
    dst.write(float4(filtered, 1.0), tid);
}
