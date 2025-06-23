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
};

struct Surface {
    float3 color;
    float ao;
    float roughness;
    float metallic;
    // float3 emission;
    float subsurface;
    float specular;
    float specular_tint;
    float anisotropic;
    float sheen;
    float sheen_tint;
    float clearcoat;
    float clearcoat_gloss;
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
constexpr constant float GAMMA = 2.2;
constexpr constant float INV_GAMMA = 1.0 / GAMMA;
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

float GTR1(float nh, float a) {
    if (a >= 1.0) return 1.0 / PI;
    float a2 = a * a;
    float t = 1.0 + (a2 - 1.0) * nh * nh;
    return (a2 - 1.0) / (PI * log(a2) * t);
}

float GTR2_aniso(float nh, float hx, float hy, float ax, float ay) {
    float t = (hx * hx) / (ax * ax) + (hy * hy) / (ay * ay) + nh * nh;
    return 1.0 / (PI * ax * ay * t * t);
}

float TrowbridgeReitzGGX(float nh, float r) {
    float a = r * r; // TODO: use r + 0.01?
    float a2 = a * a;
    float nh2 = nh * nh;
    float t2 = (nh2 * (a2 - 1.0) + 1.0) * (nh2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * t2);
}

float SmithsSchlickGGX(float nv, float nl, float r) {
    float k = (r + 1.0) * (r + 1.0) / 8.0;
    float ggx1 = nv / (nv * (1.0 - k) + k);
    float ggx2 = nl / (nl * (1.0 - k) + k);
    return ggx1 * ggx2;
}

float SmithGGX(float u, float r) {
    float a = r * r;
    float b = u * u;
    return 1.0 / (u + sqrt(a + b - a * b));
}

float SmithGGX_aniso(float u, float vx, float vy, float ax, float ay) {
    float t = vx * vx * ax * ax + vy * vy * ay * ay + u * u;
    return 1.0 / (u + sqrt(t));
}

float FresnelApprox(float u) {
    return pow(1.0 + 0.0001 - u, 5.0);
}

float luminance(float3 color) {
    return dot(color, float3(0.3, 0.6, 0.1));
}

float3 CookTorranceBRDF(float3 norm, float3 tangent, float3 bitangent, float3 lightDir, float3 viewDir, Surface surf) {
    float3 halfway = normalize(lightDir + viewDir);
    float nv = max(dot(norm, viewDir), 0.0);
    float nl = max(dot(norm, lightDir), 0.0);
    float nh = max(dot(norm, halfway), 0.0);
    float vh = max(dot(viewDir, halfway), 0.0);
    float lh = max(dot(lightDir, halfway), 0.0);
    float lum = luminance(surf.color);
    float3 tint = lum > 0.0 ? surf.color / lum : float3(1);
    float3 spec0 = mix(surf.specular * 0.08 * mix(float3(1), tint, surf.specular_tint), surf.color, surf.metallic);
    float fh = FresnelApprox(lh);
    float fl = FresnelApprox(nl);
    float fv = FresnelApprox(nv);
    float fss90 = lh * lh * surf.roughness;
    // diffuse
    float fd90 = 0.5 + 2.0 * fss90;
    float kd = mix(1.0, fd90, fl) * mix(1.0, fd90, fv);
    // float3 diffuse = kd * surf.color / PI;
    // subsurface
    float fss = mix(1.0, fss90, fl) * mix(1.0, fss90, fv);
    float ss = 1.25 * (fss * (1.0 / (nl + nv + 0.0001) - 0.5) + 0.5);
    // specular
    float aspect = sqrt(1.0 - surf.anisotropic * .9);
    float ax = max(.001, surf.roughness * surf.roughness / aspect);
    float ay = max(.001, surf.roughness * surf.roughness * aspect);
    float3 x = tangent;
    float3 y = bitangent;
    float hx = dot(halfway, x);
    float hy = dot(halfway, y);
    float lx = dot(lightDir, x);
    float ly = dot(lightDir, y);
    float vx = dot(viewDir, x);
    float vy = dot(viewDir, y);
    float D = GTR2_aniso(nh, hx, hy, ax, ay); // TrowbridgeReitzGGX(nh, surf.roughness);
    float G = SmithGGX_aniso(nl, lx, ly, ax, ay) * SmithGGX_aniso(nv, vx, vy, ax, ay); // SmithsSchlickGGX(nv, nl, surf.roughness + 0.01) / max(4.0 * nv * nl, 0.0001);
    float3 F = mix(spec0, float3(1.0), fh);
    float3 specular = D * G * F;
    // sheen
    float3 sheen = fh * surf.sheen * mix(float3(1), tint, surf.sheen_tint);
    // clearcoat
    float Dr = GTR1(nh, mix(.1, .001, surf.clearcoat_gloss));
    float Fr = mix(.04, 1.0, fh);
    float Gr = SmithGGX(nl, .25) * SmithGGX(nv, .25);
    float3 clearcoat = 0.25 * float3(surf.clearcoat) * Dr * Fr * Gr;

    return ((mix(kd, ss, surf.subsurface) * surf.color / PI + sheen) * (1.0 - surf.metallic) + specular + clearcoat) * nl;
}

float3 CalculateDirectionalLight(DirLight light, float3 norm, float3 tangent, float3 bitangent, float3 viewDir, Surface surf) {
    float3 lightDir = normalize(-light.direction);
    float3 radiance = light.color * light.intensity;

    return CookTorranceBRDF(norm, tangent, bitangent, lightDir, viewDir, surf) * radiance * clamp(dot(norm, lightDir), 0.0, 1.0);
}

float3 CalculatePointLight(PointLight light, float3 norm, float3 tangent, float3 bitangent, float3 viewDir, Surface surf, float3 fragPos) {
    float3 lightDir = normalize(light.position - fragPos);
    float dist = distance(light.position, fragPos);
    float attenuation = 1.0 / (dist * dist);
    float3 radiance = attenuation * light.color * light.intensity;

    return CookTorranceBRDF(norm, tangent, bitangent, lightDir, viewDir, surf) * radiance * clamp(dot(norm, lightDir), 0.0, 1.0);
}

vertex RasterizerData vertexMain(
    uint vertexID [[vertex_id]],
    device const VertexData* in [[buffer(0)]],
    device const CameraData& camera [[buffer(1)]],
    device const InstanceData& instance [[buffer(2)]]
) {
    RasterizerData vert;
    vert.worldPosition = instance.modelMatrix * float4(in[vertexID].position, 1.0);
    vert.worldNormal = instance.modelMatrix * float4(in[vertexID].normal, 1.0);
    vert.worldTangent = instance.modelMatrix * float4(in[vertexID].tangent, 1.0);
    vert.position = camera.projectionMatrix * camera.viewMatrix * vert.worldPosition;
    vert.uv = in[vertexID].uv;
    return vert;
}

fragment float4 fragmentMain(
    RasterizerData in [[stage_in]],
    texture2d<float, access::sample> texAlbedo [[texture(0)]],
    texture2d<float, access::sample> texNormal [[texture(1)]],
    texture2d<float, access::sample> texMetallicRoughness [[texture(2)]],
    texture2d<float, access::sample> texOcclusion [[texture(3)]],
    texture2d<float, access::sample> texEmissive [[texture(4)]],
    constant packed_float3* camPos [[buffer(0)]],
    constant float* time [[buffer(1)]]
) {
    constexpr sampler s(address::repeat, filter::linear, mip_filter::linear);
    Surface surf;
    surf.color = pow(texAlbedo.sample(s, in.uv).bgr, float3(GAMMA));
    surf.ao = 1.0; // texOcclusion.sample(s, in.uv).r;
    surf.roughness = 1.0; // texRoughness.sample(s, in.uv).g;
    surf.metallic = 0.0; // texMetallic.sample(s, in.uv).b;
    // surf.emission = float3(0.0);
    surf.subsurface = 0.0;
    surf.specular = 0.5;
    surf.specular_tint = 0.0;
    surf.anisotropic = 0.0;
    surf.sheen = 0.0;
    surf.sheen_tint = 0.5;
    surf.clearcoat = 0.0;
    surf.clearcoat_gloss = 1.0;

    float3 N = normalize(float3(in.worldNormal));
    float3 T = normalize(float3(in.worldTangent));
    float3 B = normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);
    float3 norm = normalize(TBN * normalize(texNormal.sample(s, in.uv).bgr * 2.0 - 1.0));
    float3 viewDir = normalize(*camPos - in.worldPosition.xyz);

    float3 result = float3(0.0);
    result += CalculateDirectionalLight(mainLight, norm, T, B, viewDir, surf); // result += CookTorranceBRDF(norm, lightDir, viewDir, surf) * (mainLight.color * mainLight.intensity) * clamp(dot(norm, lightDir), 0.0, 1.0);
    result += CalculatePointLight(auxLight, norm, T, B, viewDir, surf, in.worldPosition.xyz);
    result += float3(0.2) * surf.ao * surf.color;

    result = pow(result, float3(INV_GAMMA));

    return float4(result, 1.0);
}