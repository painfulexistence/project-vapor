#include <metal_stdlib>
using namespace metal;

kernel void computeMain(
    texture2d_ms<float, access::read> normalTexture [[texture(0)]],
    texture2d<float, access::write> resolvedTexture [[texture(1)]],
    constant uint& sampleCount [[buffer(0)]],
    uint2 tid [[thread_position_in_grid]]
) {
    float3 result = float3(0.0);
    uint count = 0;
    for (uint i = 0; i < sampleCount; ++i) {
        float3 norm = normalTexture.read(tid, i).rgb;
        float len = length(norm);
        if (len > 0.9 && len < 1.1) { // Caution: assumed normal is RGBA Float
            result += norm / len;
            count++;
        }
    }
    result = select(float3(0.0, 0.0, 0.0), normalize(result / float(count)), count > 0);

    resolvedTexture.write(float4(result, 1.0), tid);
}
