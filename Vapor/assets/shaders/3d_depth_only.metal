#include <metal_stdlib>
using namespace metal;

struct VertexData {
    packed_float3 position;
    packed_float2 uv;
    packed_float3 normal;
    packed_float3 tangent;
    packed_float3 bitangent;
};

struct CameraData {
    float4x4 proj;
    float4x4 view;
    float4x4 invProj;
    float near;
    float far;
};

struct InstanceData {
    float4x4 model;
    float4 color;
};

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
};

vertex RasterizerData vertexMain(
    uint vertexID [[vertex_id]],
    device const VertexData* in [[buffer(0)]],
    device const CameraData& camera [[buffer(1)]],
    device const InstanceData& instance [[buffer(2)]]
) {
    RasterizerData vert;
    float4 worldPosition = instance.model * float4(in[vertexID].position, 1.0);
    vert.position = camera.proj * camera.view * worldPosition;
    vert.uv = in[vertexID].uv;
    return vert;
}

fragment void fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> texAlbedo [[texture(0)]]
) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);

    float4 baseColor = texAlbedo.sample(s, in.uv);
    if (baseColor.a < 0.5) {
        discard_fragment();
    }
}
