#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal" // TODO: use more robust include path

struct RasterizerData {
    float4 position [[position]];
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

    float3x3 normalMatrix = transpose(inverse(float3x3(
        model[0].xyz,
        model[1].xyz,
        model[2].xyz
    )));
    // Caution: worldNormal and worldTangent are not normalized yet, and they can be affected by model scaling
    vert.worldNormal = float4(normalMatrix * float3(in[actualVertexID].normal), 0.0);
    vert.worldTangent = float4(normalMatrix * in[actualVertexID].tangent.xyz, in[actualVertexID].tangent.w);
    vert.worldPosition = model * float4(in[actualVertexID].position, 1.0);
    vert.position = camera.proj * camera.view * vert.worldPosition;
    vert.uv = in[actualVertexID].uv;
    return vert;
}

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> texAlbedo [[texture(0)]],
    texture2d<float, access::sample> texNormal [[texture(1)]],
    texture2d<float, access::sample> texMetallicRoughness [[texture(2)]],
    texture2d<float, access::sample> texOcclusion [[texture(3)]],
    texture2d<float, access::sample> texEmissive [[texture(4)]],
    texture2d<float, access::sample> texShadow [[texture(7)]],
    const device DirLight* directionalLights [[buffer(0)]],
    const device PointLight* pointLights [[buffer(1)]],
    const device Cluster* clusters [[buffer(2)]],
    constant CameraData& camera [[buffer(3)]],
    constant packed_float3* camPos [[buffer(4)]],
    constant float2& screenSize [[buffer(5)]],
    constant packed_uint3& gridSize [[buffer(6)]],
    constant float& time [[buffer(7)]]
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
    float3 norm = in.worldNormal.xyz; //normalize(TBN * normalize(texNormal.sample(s, in.uv).rgb * 2.0 - 1.0));
    float3 viewDir = normalize(*camPos - in.worldPosition.xyz);

    float3 color = N * 0.5 + 0.5;
    float len = length(color);
    color = (len > 0.9 && len < 1.1) ? color / len : float3(0.0);
    return float4(color, 1.0);

    // float2 screenUV = in.position.xy / screenSize;

    // float3 result = float3(0.0);
    // float shadowFactor = texShadow.sample(s, screenUV).r;
    // result += color * directionalLights[0].color * max(dot(norm, normalize(-directionalLights[0].direction)), 0.0) * shadowFactor;
    // // result += CalculateDirectionalLight(directionalLights[0], norm, T, B, viewDir, surf) * shadowFactor;

    // uint tileX = uint(screenUV.x * float(gridSize.x));
    // uint tileY = uint((1.0 - screenUV.y) * float(gridSize.y));
    // uint tileIndex = tileX + tileY * gridSize.x;
    // Cluster tile = clusters[tileIndex];
    // uint lightCount = tile.lightCount;
    // for (uint i = 0; i < lightCount; i++) {
    //     uint lightIndex = tile.lightIndices[i];
    //     float3 lightDir = normalize(pointLights[lightIndex].position - in.worldPosition.xyz);
    //     result += pointLights[lightIndex].color * pointLights[lightIndex].intensity * max(dot(norm, lightDir), 0.0) * shadowFactor;
    //     // result += CalculatePointLight(pointLights[lightIndex], norm, T, B, viewDir, surf, in.worldPosition.xyz);
    // }

    // return float4(result, 1.0);
}