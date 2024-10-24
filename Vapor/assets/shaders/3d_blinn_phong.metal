#include <metal_stdlib>
using namespace metal;

struct VertexData {
    packed_float3 position;
    packed_float3 normal;
    // packed_float3 tangent;
    // packed_float3 bitangent;
    float2 uv;
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
    float4 worldPosition;
    float4 worldNormal;
    // float4 worldTangent;
    // float4 worldBitangent;
};

constant float3 lightDir = float3(1.0f, 0.0f, 0.0f); // needs to be normalized
constant float3 lightAmbient = float3(0.2f, 0.2f, 0.2f);
constant float3 lightDiffuse = float3(1.0f, 1.0f, 1.0f);
constant float3 lightSpecular = float3(0.0f, 0.0f, 0.0f);
constant float lightPower = 5.0f;

constant float3 surfAmbient = float3(0.5f, 0.5f, 0.5f);
constant float3 surfDiffuse = float3(1.0f, 1.0f, 0.8f);
constant float3 surfSpecular = float3(0.5f, 0.5f, 0.5f);
constant float surfShininess = 2.0f;

vertex RasterizerData vertexMain(uint vertexID [[vertex_id]], device const VertexData* in [[buffer(0)]], device const CameraData& camera [[buffer(1)]], device const InstanceData& instance [[buffer(2)]]) {
    RasterizerData vert;
    vert.worldPosition = instance.modelMatrix * float4(in[vertexID].position, 1.0);
    vert.worldNormal = instance.modelMatrix * float4(in[vertexID].normal, 1.0);
    // vert.worldTangent = instance.modelMatrix * float4(in[vertexID].tangent, 1.0);
    // vert.worldBitangent = instance.modelMatrix * float4(in[vertexID].bitangent, 1.0);
    vert.position = camera.projectionMatrix * camera.viewMatrix * vert.worldPosition;
    vert.uv = in[vertexID].uv;
    return vert;
}

fragment half4 fragmentMain(RasterizerData in [[stage_in]], texture2d<float, access::sample> texAlbedo [[texture(0)]], texture2d<float, access::sample> texNormal [[texture(1)]], constant packed_float3* camPos [[buffer(0)]], constant float* time [[buffer(1)]]) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);
    float3 albedo = texAlbedo.sample(s, in.uv).bgr;
    // float3 norm = normalize(texNormal.sample(s, in.uv).bgr * 2.0 - 1.0);

    float3 norm = normalize(in.worldNormal.xyz);
    float3 viewDir = normalize(*camPos - in.worldPosition.xyz);
    float3 halfway = normalize(-lightDir + viewDir);

    float diff = max(dot(norm, -lightDir), 0.0);
    float3 surfColor = mix(surfDiffuse, texColor, 1.0f);

    float3 ambient = lightAmbient * surfAmbient;
    float3 diffuse = lightDiffuse * lightPower * (diff * surfColor);
    float3 specular = lightSpecular * lightPower * pow(max(dot(halfway, norm), 0.0), surfShininess) * surfSpecular;

    half3 result = half3(ambient + diffuse + specular);
    return half4(result, 1.0);
}
