#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"
#include "Res/shaders/restir_shadow_common.metal"

// Joint bilateral upsample of the half-res raw stochastic shadow factors
// (R point / G rect / B spot) to the full-res target the temporal accumulator
// and PBR consume. Sits between the half-res ReSTIR resolve and the full-res
// accumulator, mirroring how the AO chain returns to full res.
//
// For each full-res pixel, the four half-res texels of its bilinear footprint
// are weighted by bilinear x normal-cone x tangent-plane-distance against the
// full-res G-buffer. Each half texel's OWN geometry is re-read from the
// full-res G-buffer at its representative pixel (tap*2 — the pixel the
// half-res kernels actually shaded), so no separate half-res depth/normal
// targets are needed. Where no tap is geometry-compatible (thin silhouettes),
// falls back to the nearest tap so edges never go unshadowed.
kernel void computeMain(
    texture2d<float>              halfShadow    [[texture(0)]],
    texture2d<float>              depthTexture  [[texture(1)]],
    texture2d<float>              normalTexture [[texture(2)]],
    texture2d<float, access::write> outShadow   [[texture(3)]],
    constant CameraData&          camera        [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = outShadow.get_width();
    uint h = outShadow.get_height();
    if (tid.x >= w || tid.y >= h) return;
    uint halfW = halfShadow.get_width();
    uint halfH = halfShadow.get_height();

    // Nearest half texel — the fallback when geometry weights reject all taps.
    uint2 nearest = uint2(min(tid.x / 2u, halfW - 1), min(tid.y / 2u, halfH - 1));

    float depth = depthTexture.read(tid).r;
    if (depth >= 1.0) {  // sky: pass through (PBR never reads it; debug views do)
        outShadow.write(halfShadow.read(nearest), tid);
        return;
    }

    RestirSurface surf = restirReconstructSurface(tid, w, h, depth, camera);
    float3 centerN = normalize(normalTexture.read(tid).xyz);
    float planeScale = 0.02 * max(abs(surf.viewDepth), 0.5);

    // Bilinear footprint: position of this pixel in half-texel space.
    float2 halfCoord = (float2(tid) + 0.5) * 0.5 - 0.5;
    int2 base = int2(floor(halfCoord));
    float2 f = halfCoord - float2(base);
    const float2 kOffsets[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
    float kBilinear[4] = { (1.0f - f.x) * (1.0f - f.y), f.x * (1.0f - f.y),
                           (1.0f - f.x) * f.y,          f.x * f.y };

    float3 sum = float3(0.0);
    float weightSum = 0.0;
    for (uint i = 0; i < 4; i++) {
        int2 tap = clamp(base + int2(kOffsets[i]), int2(0), int2(halfW - 1, halfH - 1));
        uint2 sp = uint2(tap) * 2u;  // the full-res pixel this half texel shaded

        float tapDepth = depthTexture.read(sp).r;
        if (tapDepth >= 1.0) continue;
        RestirSurface tapSurf = restirReconstructSurface(sp, w, h, tapDepth, camera);
        float3 tapN = normalize(normalTexture.read(sp).xyz);

        float wNormal = pow(max(0.0, dot(centerN, tapN)), 32.0);
        float wPlane = exp(-abs(dot(tapSurf.worldPos - surf.worldPos, centerN)) / planeScale);
        float weight = max(kBilinear[i], 1e-3) * wNormal * wPlane;

        sum += halfShadow.read(uint2(tap)).rgb * weight;
        weightSum += weight;
    }

    float3 result = weightSum > 1e-5 ? sum / weightSum
                                     : halfShadow.read(nearest).rgb;
    outShadow.write(float4(result, 1.0), tid);
}
