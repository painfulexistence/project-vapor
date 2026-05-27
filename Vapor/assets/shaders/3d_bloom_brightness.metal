#include <metal_stdlib>
using namespace metal;

// Brightness extraction pass for bloom
// Extracts pixels above a luminance threshold

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
    constant float& threshold [[buffer(0)]]
) {
    constexpr sampler s(address::clamp_to_edge, filter::linear);
    float3 color = texScreen.sample(s, in.uv).rgb;

    // Calculate luminance using standard coefficients
    float brightness = dot(color, float3(0.2126, 0.7152, 0.0722));

    // Soft threshold with smooth falloff
    float softThreshold = threshold * 0.8;
    float contribution = max(0.0, brightness - softThreshold) / max(brightness, 0.0001);

    // Only output colors above threshold
    if (brightness > threshold) {
        return float4(color * contribution, 1.0);
    }
    return float4(0.0, 0.0, 0.0, 1.0);
}
