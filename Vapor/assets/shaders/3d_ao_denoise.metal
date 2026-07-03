#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal" // TODO: use more robust include path

constant float kAtrousKernel1D[5] = { 1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0 };

// Edge-aware à-trous wavelet filter for RT AO (ADR-008 step 3).
// src is RG16F (ao, view-space depth); the depth channel rides along so each
// iteration can edge-stop without re-reading the depth buffer. Run iteratively
// with stride 1, 2, (4, ...); the final iteration may write to a single-channel
// target — the depth channel is simply dropped.
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

    float2 center = src.read(tid).rg;
    float centerZ = center.g;
    float3 centerN = normalize(normalTexture.read(tid).xyz);

    float sum = 0.0;
    float weightSum = 0.0;
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int2 tap = int2(tid) + int2(dx, dy) * int(stride);
            if (tap.x < 0 || tap.y < 0 || tap.x >= int(w) || tap.y >= int(h)) continue;

            float2 s = src.read(uint2(tap)).rg;
            float3 n = normalize(normalTexture.read(uint2(tap)).xyz);

            float wKernel = kAtrousKernel1D[dx + 2] * kAtrousKernel1D[dy + 2];
            float wZ = exp(-abs(s.g - centerZ) / (0.05 * max(abs(centerZ), 1.0)));
            float wN = pow(max(0.0, dot(centerN, n)), 32.0);

            float weight = wKernel * wZ * wN;
            sum += s.r * weight;
            weightSum += weight;
        }
    }
    float ao = weightSum > 1e-6 ? sum / weightSum : center.r;
    dst.write(float4(ao, centerZ, 0.0, 0.0), tid);
}
