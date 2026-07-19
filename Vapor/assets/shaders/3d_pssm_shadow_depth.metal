#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"

struct ShadowDepthVert {
    float4 position [[position]];
    float2 uv;
    MaterialData material;
};

vertex ShadowDepthVert vertexMain(
    uint vertexID [[vertex_id]],
    constant float4x4& lightSpaceMatrix [[buffer(0)]],
    constant MaterialData* materials [[buffer(1)]],
    constant InstanceData* instances [[buffer(2)]],
    device const VertexData* in [[buffer(3)]],
    constant uint& instanceID [[buffer(4)]]
) {
    ShadowDepthVert vert;
    uint actualVertexID = instances[instanceID].vertexOffset + vertexID;
    float4 worldPos = instances[instanceID].model * float4(float3(in[actualVertexID].position), 1.0);
    vert.position = lightSpaceMatrix * worldPos;
    vert.uv = float2(in[actualVertexID].uv);
    vert.material = materials[instances[instanceID].materialID];
    return vert;
}

fragment void fragmentMain(
    ShadowDepthVert in [[stage_in]],
    texture2d<float, access::sample> texAlbedo [[texture(0)]]
) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);
    float4 baseColor = texAlbedo.sample(s, in.uv);
    if (in.material.emissiveFactor.a > 0.0 && baseColor.a * in.material.baseColorFactor.a < in.material.emissiveFactor.a) {
        discard_fragment();
    }
}
