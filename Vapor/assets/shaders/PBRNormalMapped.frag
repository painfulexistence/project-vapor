#version 450

layout(location = 0) in vec3 frag_pos;
layout(location = 1) in vec2 tex_uv;
layout(location = 2) in mat3 TBN;
layout(location = 0) out vec4 Color;
layout(push_constant) uniform PushConstantBlock {
    vec3 cam_pos;
    float time;
};
layout(binding = 1) uniform sampler2D base_map;
layout(binding = 2) uniform sampler2D normal_map;
// layout(binding = 3) uniform sampler2D ao_map;
// layout(binding = 4) uniform sampler2D roughness_map;
// layout(binding = 5) uniform sampler2D metallic_map;

struct Surface {
    vec3 color;
    float ao;
    float roughness;
    float metallic;
};

struct DirLight {
    vec3 direction;
    vec3 color;
    float intensity;
};

struct PointLight {
    vec3 position;
    vec3 attenuation;
    vec3 color;
    float intensity;
};

const float PI = 3.1415927;
const float gamma = 2.2;

vec3 CookTorranceBRDF(vec3 norm, vec3 lightDir, vec3 viewDir, Surface surf);

float TrowbridgeReitzGGX(float nh, float r);

float SmithsSchlickGGX(float nv, float nl, float r);

vec3 FresnelSchlick(float nh, vec3 f0);

vec3 SurfaceColor();

vec3 CalculateDirectionalLight(DirLight light, vec3 norm, vec3 viewDir, Surface surf) {
    vec3 lightDir = normalize(-light.direction);
    vec3 radiance = light.color * light.intensity;
    return CookTorranceBRDF(norm, lightDir, viewDir, surf) * radiance * clamp(dot(norm, lightDir), 0.0, 1.0);
}

vec3 CalculatePointLight(PointLight light, vec3 norm, vec3 viewDir, Surface surf) {
    vec3 lightDir = normalize(light.position - frag_pos);
    float dist = distance(light.position, frag_pos);
    float attenuation = 1.0 / (dist * dist);
    vec3 radiance = attenuation * light.color * light.intensity;
    return CookTorranceBRDF(norm, lightDir, viewDir, surf) * radiance * clamp(dot(norm, lightDir), 0.0, 1.0);
}

vec3 CookTorranceBRDF(vec3 norm, vec3 lightDir, vec3 viewDir, Surface surf) {
    vec3 halfway = normalize(lightDir + viewDir);
    float nv = clamp(dot(norm, viewDir), 0.0, 1.0);
    float nl = clamp(dot(norm, lightDir), 0.0, 1.0);
    float nh = clamp(dot(norm, halfway), 0.0, 1.0);

    float D = TrowbridgeReitzGGX(nh, surf.roughness + 0.01);
    float G = SmithsSchlickGGX(nv, nl, surf.roughness + 0.01);
    vec3 F = FresnelSchlick(nh, mix(vec3(0.04), surf.color, surf.metallic));

    vec3 specular = D * F * G / max(4.0 * nv * nl, 0.0001);
    vec3 kd = (1.0 - surf.metallic) * (vec3(1.0) - F);
    vec3 diffuse = kd * surf.color / PI;

    return diffuse + specular;
}

float TrowbridgeReitzGGX(float nh, float r) {
    float r2 = r * r;
    float nh2 = nh * nh;
    float nhr2 = (nh2 * (r2 - 1) + 1) * (nh2 * (r2 - 1) + 1);
    return r2 / (PI * nhr2);
}

float SmithsSchlickGGX(float nv, float nl, float r) {
    float k = (r + 1.0) * (r + 1.0) / 8.0;
    float ggx1 = nv / (nv * (1.0 - k) + k);
    float ggx2 = nl / (nl * (1.0 - k) + k);
    return ggx1 * ggx2;
}

vec3 FresnelSchlick(float nh, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - nh, 5.0);
}

vec3 SurfaceColor() {
    vec3 texColor = pow(texture(base_map, tex_uv).rgb, vec3(gamma));
    return texColor;
}

float SurfaceAO() {
    return 0.0; // texture(ao_map, tex_uv).r;
}

float SurfaceRoughness() {
    return 0.5; // texture(roughness_map, tex_uv).r;
}

float SurfaceMetallic() {
    return 0.0; // texture(metallic_map, tex_uv).r;
}

DirLight main_light = DirLight(vec3(0.0, 1.0, 0.0), vec3(1.0, 1.0, 1.0), 10.0);
PointLight aux_lights[1] = {
    PointLight(vec3(0.0, 1.0, 0.0), vec3(0.5), vec3(1.0, 0.0, 0.0), 0.2),
};

void main() {
    vec3 texNorm = texture(normal_map, tex_uv).rgb * 2.0 - 1.0;
    vec3 norm = normalize(TBN * texNorm);
    vec3 viewDir = normalize(cam_pos - frag_pos);

    Surface surf = Surface(
        SurfaceColor(),
        SurfaceAO(),
        SurfaceRoughness(),
        SurfaceMetallic()
    );

    vec3 result = vec3(0.0);
    result += CalculateDirectionalLight(main_light, norm, viewDir, surf);
    result += CalculatePointLight(aux_lights[0], norm, viewDir, surf);
    // result += CalculatePointLight(aux_lights[1], norm, viewDir, surf);
    // result += CalculatePointLight(aux_lights[2], norm, viewDir, surf);
    // result += CalculatePointLight(aux_lights[3], norm, viewDir, surf);
    // result += vec3(0.2) * (1.0 - surf.ao) * surf.color;

    result = pow(result, vec3(1.0 / gamma));

    Color = vec4(result, 1.0);
}
