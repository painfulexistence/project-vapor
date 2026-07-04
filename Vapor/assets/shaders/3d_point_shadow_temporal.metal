#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"

// Temporal accumulation denoiser for stochastic point light shadows.
// Velocity reprojection + variance clamping + 15%/85% blend.
kernel void computeMain(
    texture2d<float>              currentShadow   [[texture(0)]],
    texture2d<float>              historyShadow   [[texture(1)]],
    texture2d<float>              velocityTexture [[texture(2)]],
    texture2d<float, access::write> outputShadow  [[texture(3)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint w = outputShadow.get_width();
    uint h = outputShadow.get_height();
    if (tid.x >= w || tid.y >= h) return;

    float2 screenSize = float2(w, h);
    float2 uv = (float2(tid) + 0.5) / screenSize;

    float current = currentShadow.read(tid).r;

    // Velocity from 3d_velocity.metal is (currNDC - prevNDC) * 0.5 in y-up NDC
    // space; convert to y-down texture UV by negating the y component.
    float2 velocity = velocityTexture.read(tid).rg;
    float2 prevUV = uv - float2(velocity.x, -velocity.y);

    // Variance clamping neighbourhood (3x3)
    float mean = 0.0;
    float sq   = 0.0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            uint2 nc = uint2(clamp(int2(tid) + int2(dx, dy), int2(0), int2(w-1, h-1)));
            float v = currentShadow.read(nc).r;
            mean += v;
            sq   += v * v;
        }
    }
    mean /= 9.0;
    float variance = max(sq / 9.0 - mean * mean, 0.0);
    float stddev = sqrt(variance);

    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    float history = historyShadow.sample(s, prevUV).r;

    // Clamp history into current neighbourhood bbox
    history = clamp(history, mean - stddev * 1.5, mean + stddev * 1.5);

    // Reject history when reprojected UV is out of bounds
    bool valid = all(prevUV >= 0.0) && all(prevUV <= 1.0);
    float blendFactor = valid ? 0.15 : 1.0; // 15% current, 85% history

    float result = mix(history, current, blendFactor);
    outputShadow.write(float4(result, 0.0, 0.0, 1.0), tid);
}
