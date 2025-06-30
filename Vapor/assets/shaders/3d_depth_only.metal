#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal" // TODO: use more robust include path

struct RasterizerData {
    float4 position [[position]];
    float4 normal;
    float2 uv;
    float4 worldPosition;
    float4 worldNormal;
    float4 worldTangent;
};

vertex RasterizerData vertexMain(
    uint vertexID [[vertex_id]],
    constant CameraData& camera [[buffer(0)]],
    constant InstanceData* instances [[buffer(1)]],
    device const VertexData* in [[buffer(2)]],
    constant uint& instanceID [[buffer(3)]]
) {
    RasterizerData vert;
    uint actualVertexID = instances[instanceID].vertexOffset + vertexID;
    float4x4 model = instances[instanceID].model;
    float3x3 model33 = float3x3(model[0].xyz, model[1].xyz, model[2].xyz);
    float3x3 normalMatrix = transpose(inverse(model33));
    // Caution: worldNormal and worldTangent are not normalized yet, and they can be affected by model scaling
    vert.worldNormal = float4(normalMatrix * float3(in[actualVertexID].normal.xyz), 0.0);
    vert.worldTangent = float4(normalMatrix * in[actualVertexID].tangent.xyz, in[actualVertexID].tangent.w);
    vert.worldPosition = model * float4(in[actualVertexID].position, 1.0);
    vert.position = camera.proj * camera.view * vert.worldPosition;
    vert.uv = in[vertexID].uv;
    return vert;
}

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> texAlbedo [[texture(0)]],
    texture2d<float, access::sample> texNormal [[texture(1)]]
) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);

    float4 baseColor = texAlbedo.sample(s, in.uv);
    if (baseColor.a < 0.5) {
        discard_fragment();
    }

    float3 N = normalize(float3(in.worldNormal));
    float3 T = normalize(float3(in.worldTangent));
    T = normalize(T - dot(T, N) * N);
    float3 B = normalize(cross(N, T) * in.worldTangent.w);
    float3x3 TBN = float3x3(T, B, N);
    float3 normal = texAlbedo.sample(s, in.uv).rgb * 2.0 - 1.0;
    normal.y = 1.0 - normal.y;
    float3 norm = normalize(TBN * normal);
    return float4(N, 1.0);
}
