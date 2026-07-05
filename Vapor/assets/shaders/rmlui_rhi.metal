#include <metal_stdlib>
using namespace metal;

// RmlUI geometry (RHI renderer, Metal backend). RHI_Metal pipelines carry no
// vertex descriptor, so vertices are fetched raw from a device pointer (the
// same pattern as 2d_batch.metal). Buffer table:
//   buffer(0) = premultiplied mvp (RHI::setVertexBytes(&mvp, 64, 0))
//   buffer(1) = Rml::Vertex array   (RHI::bindVertexBuffer(vb, 1, 0))
// Rml::Vertex is tightly packed (20 bytes): float2 position, RGBA8 colour,
// float2 tex_coord — hence the packed_float2 members.

struct RmlVertexIn {
    packed_float2 position;
    uchar4        color;
    packed_float2 uv;
};

struct RmlVertexOut {
    float4 position [[position]];
    float4 color;
    float2 uv;
};

vertex RmlVertexOut vertexMain(
    uint vertexID [[vertex_id]],
    constant float4x4& mvp [[buffer(0)]],
    device const RmlVertexIn* vertices [[buffer(1)]]
) {
    RmlVertexIn v = vertices[vertexID];
    RmlVertexOut out;
    out.position = mvp * float4(float2(v.position), 0.0, 1.0);
    out.color = float4(v.color) / 255.0;
    out.uv = float2(v.uv);
    return out;
}

fragment float4 fragmentMain(
    RmlVertexOut in [[stage_in]],
    texture2d<float, access::sample> tex [[texture(0)]],
    sampler smp [[sampler(0)]]
) {
    return in.color * tex.sample(smp, in.uv);
}
