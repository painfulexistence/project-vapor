#include <metal_stdlib>
using namespace metal;

// Bloom composite pass
// Combines the original scene with the bloom result

constant float2 ndcVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
};

vertex RasterizerData vertexMain(uint vertexID [[vertex_id]]) {
    RasterizerData vert;
    vert.position = float4(ndcVerts[vertexID], 0.0, 1.0);
    vert.uv = ndcVerts[vertexID] * 0.5 + 0.5;
    vert.uv.y = 1.0 - vert.uv.y;
    return vert;
}

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> texScreen [[texture(0)]],
    texture2d<float, access::sample> texBloom [[texture(1)]],
    constant float& bloomStrength [[buffer(0)]]
) {
    constexpr sampler s(address::clamp_to_edge, filter::linear);

    float3 sceneColor = texScreen.sample(s, in.uv).rgb;
    float3 bloomColor = texBloom.sample(s, in.uv).rgb;

    // Additive blend with strength control
    float3 result = sceneColor + bloomColor * bloomStrength;

    return float4(result, 1.0);
}
