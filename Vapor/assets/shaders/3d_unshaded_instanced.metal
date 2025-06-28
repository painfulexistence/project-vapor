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
    float4x4 projectionMatrix;
    float4x4 viewMatrix;
};

struct InstanceData {
    float4x4 modelMatrix;
    float4 color;
};

struct RasterizerData {
    float4 position [[position]];
    float2 uv;
};

vertex RasterizerData vertexMain(uint vertexID [[vertex_id]], uint instanceID [[instance_id]], device const VertexData* in [[buffer(0)]], device const CameraData& cameraData [[buffer(1)]], device const InstanceData* instanceData [[buffer(2)]]) {
    RasterizerData vert;
    vert.position = cameraData.projectionMatrix * cameraData.viewMatrix * instanceData[instanceID].modelMatrix * float4(in[vertexID].position, 1.0);
    vert.uv = in[vertexID].uv;
    return vert;
}

fragment half4 fragmentMain(RasterizerData in [[stage_in]], texture2d<half, access::sample> tex [[texture(0)]], constant float* time [[buffer(0)]]) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);
    return half4(tex.sample(s, in.uv).rgb, 1.0);
}
