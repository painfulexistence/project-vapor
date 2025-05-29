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
    float4 worldPosition;
    float4 worldNormal;
    float4 worldTangent;
    float4 worldBitangent;
};

struct Surface {
    float3 color;
    float ao;
    float roughness;
    float metallic;
};

struct DirLight {
    float3 direction;
    float3 color;
    float intensity;
};

struct PointLight {
    float3 position;
    float3 color;
    float intensity;
};

struct LightData {
    DirLight main_light;
    PointLight aux_lights[6];
    int aux_light_count;
};

constexpr constant float PI = 3.1415927;
constexpr constant float gamma = 2.2;
constant DirLight mainLight = {
    float3(0.0f, -1.0f, 0.0f),
    float3(1.0f, 1.0f, 1.0f),
    10.0f,
};
constant PointLight auxLight = {
    float3(0.0f, 1.0f, 0.0f),
    float3(1.0f, 0.0f, 0.0f),
    3.2f,
};

float TrowbridgeReitzGGX(float nh, float r) {
    float r2 = r * r;
    float a2 = r2 * r2;
    float nh2 = nh * nh;
    float nhr2 = (nh2 * (a2 - 1.0) + 1.0) * (nh2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * nhr2);
}

float SmithsSchlickGGX(float nv, float nl, float r) {
    float k = (r + 1.0) * (r + 1.0) / 8.0;
    float ggx1 = nv / (nv * (1.0 - k) + k);
    float ggx2 = nl / (nl * (1.0 - k) + k);
    return ggx1 * ggx2;
}

float3 FresnelSchlick(float vh, float3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);
}

float3 CookTorranceBRDF(float3 norm, float3 lightDir, float3 viewDir, Surface surf) {
    float3 halfway = normalize(lightDir + viewDir);
    float nv = max(dot(norm, viewDir), 0.0);
    float nl = max(dot(norm, lightDir), 0.0);
    float nh = max(dot(norm, halfway), 0.0);
    float vh = max(dot(viewDir, halfway), 0.0);

    float D = TrowbridgeReitzGGX(nh, surf.roughness + 0.01);
    float G = SmithsSchlickGGX(nv, nl, surf.roughness + 0.01);
    float3 F = FresnelSchlick(vh, mix(float3(0.04), surf.color, surf.metallic));

    float3 specular = D * F * G / max(4.0 * nv * nl, 0.0001);
    float3 kd = (1.0 - surf.metallic) * (float3(1.0) - F);
    float3 diffuse = kd * surf.color / PI;

    return diffuse + specular;
}

float3 CalculateDirectionalLight(DirLight light, float3 norm, float3 viewDir, Surface surf) {
    float3 lightDir = normalize(-light.direction);
    float3 radiance = light.color * light.intensity;

    return CookTorranceBRDF(norm, lightDir, viewDir, surf) * radiance * clamp(dot(norm, lightDir), 0.0, 1.0);
}

float3 CalculatePointLight(PointLight light, float3 norm, float3 viewDir, Surface surf, float3 fragPos) {
    float3 lightDir = normalize(light.position - fragPos);
    float dist = distance(light.position, fragPos);
    float attenuation = 1.0 / (dist * dist);
    float3 radiance = attenuation * light.color * light.intensity;

    return CookTorranceBRDF(norm, lightDir, viewDir, surf) * radiance * clamp(dot(norm, lightDir), 0.0, 1.0);
}

vertex RasterizerData vertexMain(uint vertexID [[vertex_id]], device const VertexData* in [[buffer(0)]], device const CameraData& camera [[buffer(1)]], device const InstanceData& instance [[buffer(2)]]) {
    RasterizerData vert;
    vert.worldPosition = instance.modelMatrix * float4(in[vertexID].position, 1.0);
    vert.worldNormal = instance.modelMatrix * float4(in[vertexID].normal, 1.0);
    vert.worldTangent = instance.modelMatrix * float4(in[vertexID].tangent, 1.0);
    vert.worldBitangent = instance.modelMatrix * float4(in[vertexID].bitangent, 1.0);
    vert.position = camera.projectionMatrix * camera.viewMatrix * vert.worldPosition;
    vert.uv = in[vertexID].uv;
    return vert;
}

fragment float4 fragmentMain(RasterizerData in [[stage_in]], texture2d<float, access::sample> texAlbedo [[texture(0)]], texture2d<float, access::sample> texNormal [[texture(1)]], texture2d<float, access::sample> texAO [[texture(2)]], texture2d<float, access::sample> texRoughness [[texture(3)]], texture2d<float, access::sample> texMetallic [[texture(4)]], constant packed_float3* camPos [[buffer(0)]], constant float* time [[buffer(1)]]) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);
    Surface surf;
    surf.color = pow(texAlbedo.sample(s, in.uv).bgr, float3(gamma));
    surf.ao = 0.9; // texAO.sample(s, in.uv).r;
    surf.roughness = 1.0; // texRoughness.sample(s, in.uv).r;
    surf.metallic = 0.0; // texMetallic.sample(s, in.uv).r;

    float3 N = normalize(float3(in.worldNormal));
    float3 T = normalize(float3(in.worldTangent));
    float3 B = normalize(float3(in.worldBitangent));
    float3x3 TBN = float3x3(T, B, N);
    float3 norm = normalize(TBN * normalize(texNormal.sample(s, in.uv).bgr * 2.0 - 1.0));
    float3 viewDir = normalize(*camPos - in.worldPosition.xyz);

    float3 result = float3(0.0);
    result += CalculateDirectionalLight(mainLight, norm, viewDir, surf); // result += CookTorranceBRDF(norm, lightDir, viewDir, surf) * (mainLight.color * mainLight.intensity) * clamp(dot(norm, lightDir), 0.0, 1.0);
    result += CalculatePointLight(auxLight, norm, viewDir, surf, in.worldPosition.xyz);
    result += float3(0.2) * surf.ao * surf.color;

    result = pow(result, float3(1.0 / gamma));

    return float4(result, 1.0);
}