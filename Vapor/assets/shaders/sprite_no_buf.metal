#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

constant float2 positions[] = {
    float2(-0.5f, 0.5f), // top-left
    float2(-0.5f, -0.5f), // bottom-left
    float2(0.5f, 0.5f), // top-right
    float2(0.5f, 0.5f), // top-right
    float2(-0.5f, -0.5f), // bottom-left
    float2(0.5f, -0.5f) // bottom-right
};

constant float2 uvs[] = {
    float2(0.0f, 0.0f),
    float2(0.0f, 1.0f),
    float2(1.0f, 0.0f),
    float2(1.0f, 0.0f),
    float2(0.0f, 1.0f),
    float2(1.0f, 1.0f)
};

vertex VertexOut vertexMain(uint vertexID [[vertex_id]]) {
    VertexOut vert;
    vert.position = float4(positions[vertexID], 0.0, 1.0);
    vert.uv = uvs[vertexID];
    return vert;
}

fragment half4 fragmentMain(VertexOut in [[stage_in]], texture2d<half, access::sample> tex [[texture(0)]]) {
    constexpr sampler s(address::repeat, filter::linear);
    return half4(tex.sample(s, in.uv).rgb, 1.0);
}
