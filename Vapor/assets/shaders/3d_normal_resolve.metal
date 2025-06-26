#include <metal_stdlib>
using namespace metal;

kernel void computeMain(
    texture2d_ms<float, access::read> normalTexture [[texture(0)]],
    texture2d<float, access::write> resolvedTexture [[texture(1)]],
    uint2 tid [[thread_position_in_grid]]
) {
    uint2 coord = uint2(tid);

    float3 avgNormal = float3(0.0);
    uint validSamples = 0;
    for (uint i = 0; i < 4; ++i) {
        float3 normal = normalTexture.read(coord, i).rgb;
        if (length(normal) > 0.1) {
            avgNormal += normal;
            validSamples++;
        }
    }
    if (validSamples > 0) {
        avgNormal = normalize(avgNormal / float(validSamples));
    } else {
        avgNormal = float3(0.0, 0.0, 1.0);
    }
    resolvedTexture.write(float4(avgNormal * 0.5 + 0.5, 1.0), coord);
}
