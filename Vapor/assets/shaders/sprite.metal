#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut vertexMain(uint vertexID [[vertex_id]], device const float2* positions [[buffer(0)]], device const float2* uvs [[buffer(1)]]) {
    VertexOut vert;
    vert.position = float4(positions[vertexID], 1.0);
    vert.uv = uvs[vertexID];
    return vert;
}

fragment half4 fragmentMain(VertexOut in [[stage_in]], texture2d<half, access::sample> tex [[texture(0)]]) {
    constexpr sampler s(address::repeat, filter::linear);
    return half4(tex.sample(s, in.uv).bgr, 1.0);
}
