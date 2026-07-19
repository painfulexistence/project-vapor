#include <metal_stdlib>
using namespace metal;
#include "Res/shaders/3d_common.metal"

struct ShadowDepthVert {
    float4 position [[position]];
    float2 uv;
    float4 baseColorFactor;  // alpha test; passing these two avoids 112B inter-stage overflow
    float alphaCutoff;       // MASK cutoff (materials[i].emissiveFactor.a); 0 = disabled
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
    vert.baseColorFactor = materials[instances[instanceID].materialID].baseColorFactor;
    vert.alphaCutoff = materials[instances[instanceID].materialID].emissiveFactor.a;// .a = MASK cutoff
    return vert;
}

fragment void fragmentMain(
    ShadowDepthVert in [[stage_in]],
    texture2d<float, access::sample> texAlbedo [[texture(0)]]
) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);
    // Mip 0, not the auto mip: in the shadow map the caster spans few texels,
    // so a coarse mip's averaged alpha drops below the cutoff and the foliage
    // casts no shadow at all. Full-res alpha keeps the leaf mask.
    float4 baseColor = texAlbedo.sample(s, in.uv, level(0));
    if (in.alphaCutoff > 0.0 && baseColor.a * in.baseColorFactor.a < in.alphaCutoff) {
        discard_fragment();
    }
}
