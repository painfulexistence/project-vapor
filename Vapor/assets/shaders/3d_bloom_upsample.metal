#include <metal_stdlib>
using namespace metal;

// Physically-based bloom upsample pass
// Uses 3x3 tent filter with additive blending to accumulate bloom

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

// 3x3 tent filter kernel weights (for smooth upsampling)
constant float weights[9] = {
    1.0, 2.0, 1.0,
    2.0, 4.0, 2.0,
    1.0, 2.0, 1.0
};

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> texInput [[texture(0)]],
    texture2d<float, access::sample> texBlend [[texture(1)]]
) {
    constexpr sampler s(address::clamp_to_edge, filter::linear);

    float2 texelSize = 1.0 / float2(texInput.get_width(), texInput.get_height());
    float3 col = float3(0.0);

    // Apply 3x3 tent filter for upsampling
    int idx = 0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float2 offset = float2(x, y) * texelSize;
            col += texInput.sample(s, in.uv + offset).rgb * weights[idx++];
        }
    }
    col /= 16.0;

    // Add the higher resolution blend texture (accumulative bloom)
    float3 blend = texBlend.sample(s, in.uv).rgb;

    return float4(col + blend, 1.0);
}
