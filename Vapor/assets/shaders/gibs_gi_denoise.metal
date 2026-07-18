#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"

constant float kAtrousKernel1D[5] = { 1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0 };

// SVGF-lite step 2 for GIBS GI: edge-aware a-trous wavelet filter over the
// temporally-accumulated GI (RGB port of 3d_ao_denoise.metal). This is what
// blends away the visible surfel-disc seams: taps across a geometric edge are
// rejected by the depth/normal stops, taps across a disc seam on the same
// surface are averaged. Run iteratively with stride 1, 2, (4, ...).
//
// src is RGBA16F (RGB = GI, A = view-space depth). Normals come from the
// full-res normal RT (the GI history does not pack them, unlike the AO chain).
kernel void computeMain(
    texture2d<float> src [[texture(0)]],
    texture2d<float, access::write> dst [[texture(1)]],
    texture2d<float> normalTexture [[texture(2)]],
    constant uint& stride [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = dst.get_width();
    uint h = dst.get_height();
    if (tid.x >= w || tid.y >= h) {
        return;
    }

    uint2 fullDim = uint2(normalTexture.get_width(), normalTexture.get_height());
    uint2 scale = max(fullDim / uint2(w, h), 1u);

    float4 center = src.read(tid);
    float centerZ = center.a;
    uint2 centerFull = min(tid * scale, fullDim - 1);
    float3 centerN = normalize(normalTexture.read(centerFull).xyz);

    float3 sum = float3(0.0);
    float weightSum = 0.0;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int2 tap = int2(tid) + int2(dx, dy) * int(stride);
            if (tap.x < 0 || tap.y < 0 || tap.x >= int(w) || tap.y >= int(h)) continue;

            float4 sVal = src.read(uint2(tap));
            uint2 tapFull = min(uint2(tap) * scale, fullDim - 1);
            float3 n = normalize(normalTexture.read(tapFull).xyz);

            float wKernel = kAtrousKernel1D[dx + 2] * kAtrousKernel1D[dy + 2];
            float wZ = exp(-abs(sVal.a - centerZ) / (0.05 * max(abs(centerZ), 1.0)));
            float wN = pow(max(0.0, dot(centerN, n)), 32.0);

            float weight = wKernel * wZ * wN;
            sum += sVal.rgb * weight;
            weightSum += weight;
        }
    }
    float3 gi = weightSum > 1e-6 ? sum / weightSum : center.rgb;
    dst.write(float4(gi, centerZ), tid);
}
