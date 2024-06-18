#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    half3 color;
};

constant float2 positions[] = {
    float2(0.0f, 1.0f),
    float2(1.0f, -1.0f),
    float2(-1.0f, -1.0f),
    float2(0.5f, 0.0f),
    float2(0.0f, -1.0f),
    float2(-0.5f, 0.0f)
};

constant half3 colors[] = {
    half3(0.75f, 0.75f, 0.0f),
    half3(0.5f, 0.5f, 0.0f),
    half3(1.0f, 1.0f, 0.0f),
    half3(0.0f, 0.0f, 0.0f),
    half3(0.0f, 0.0f, 0.0f),
    half3(0.0f, 0.0f, 0.0f)
};

vertex VertexOut vertexMain(uint vertexID [[vertex_id]]) {
    VertexOut vert;
    vert.position = float4(positions[vertexID] / 2.0, 0.0, 1.0);
    vert.color = colors[vertexID];
    return vert;
}

fragment half4 fragmentMain(VertexOut in [[stage_in]]) {
    return half4(in.color, 1.0);
}
