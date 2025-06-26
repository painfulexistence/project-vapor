#version 450

layout(location = 0) in vec3 frag_pos;
layout(location = 1) in vec2 tex_uv;
layout(location = 2) in vec3 T;
layout(location = 3) in vec3 N;
layout(location = 0) out vec4 Color;

const uint MAX_LIGHTS_PER_TILE = 256; // Must match the definition in graphics.hpp

struct Cluster {
    vec4 min;
    vec4 max;
    uint lightCount;
    uint lightIndices[MAX_LIGHTS_PER_TILE];
};

struct DirLight {
    vec3 direction;
    float _pad1;
    vec3 color;
    float _pad2;
    float intensity;
    // float _pad3[3];
};

struct PointLight {
    vec3 position;
    float _pad1;
    vec3 color;
    float _pad2;
    float intensity;
    float radius;
    // float _pad3[2];
};

layout(push_constant) uniform PushConstants {
    vec3 camPos;
};
layout(std430, set = 0, binding = 2) readonly buffer DirLightBuffer {
    DirLight directional_lights[];
};
layout(std430, set = 0, binding = 3) readonly buffer PointLightBuffer {
    PointLight point_lights[];
};
layout(std140, set = 0, binding = 4) uniform LightCullData {
    vec2 screenSize;
    vec2 _pad1;
    uvec3 gridSize;
    uint lightCount;
};
layout(std430, set = 0, binding = 5) readonly buffer ClusterBuffer {
    Cluster clusters[];
};
layout(set = 1, binding = 0) uniform sampler2D base_map;
layout(set = 1, binding = 1) uniform sampler2D normal_map;
// layout(set = 1, binding = 2) uniform sampler2D metallic_roughness_map;
// layout(set = 1, binding = 3) uniform sampler2D occlusion_map;
// layout(set = 1, binding = 4) uniform sampler2D emission_map;
// layout(set = 1, binding = 10) uniform sampler2D env_map;

struct Surface {
    vec3 color;
    float ao;
    float roughness;
    float metallic;
    // vec3 emission;
    float subsurface;
    float specular;
    float specular_tint;
    float anisotropic;
    float sheen;
    float sheen_tint;
    float clearcoat;
    float clearcoat_gloss;
};

const float PI = 3.1415927;
const float GAMMA = 2.2;
const float INV_GAMMA = 1.0 / GAMMA;

vec3 CookTorranceBRDF(vec3 norm, vec3 tangent, vec3 bitangent, vec3 lightDir, vec3 viewDir, Surface surf);

float TrowbridgeReitzGGX(float nh, float r);

float SmithsSchlickGGX(float nv, float nl, float r);

vec3 CalculateDirectionalLight(DirLight light, vec3 norm, vec3 tangent, vec3 bitangent, vec3 viewDir, Surface surf) {
    vec3 lightDir = normalize(-light.direction);
    vec3 radiance = light.color * light.intensity;
    return CookTorranceBRDF(norm, tangent, bitangent, lightDir, viewDir, surf) * radiance * max(dot(norm, lightDir), 0.0);
}

vec3 CalculatePointLight(PointLight light, vec3 norm, vec3 tangent, vec3 bitangent, vec3 viewDir, Surface surf) {
    vec3 lightDir = normalize(light.position - frag_pos);
    float dist = distance(light.position, frag_pos);
    float attenuation = 1.0 / (dist * dist);
    attenuation *= smoothstep(light.radius, light.radius * 0.8, dist);
    vec3 radiance = attenuation * light.color * light.intensity;
    return CookTorranceBRDF(norm, tangent, bitangent, lightDir, viewDir, surf) * radiance * max(dot(norm, lightDir), 0.0);
}

// vec3 CalculateIBL(vec3 norm, vec3 viewDir, Surface surf) {
//     vec3 reflectDir = reflect(-viewDir, norm);
//     float theta = -acos(reflectDir.y);
//     float phi = atan(reflectDir.z, reflectDir.x);
//     vec2 uv = fract(vec2(phi, theta) / vec2(2.0 * PI, PI) + vec2(0.5, 0.0));
//     vec3 env = texture(env_map, uv).rgb;
//     return env * surf.color;
// }

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

float luminance(vec3 color) {
    return dot(color, vec3(0.3, 0.6, 0.1));
}

vec3 CookTorranceBRDF(vec3 norm, vec3 tangent, vec3 bitangent, vec3 lightDir, vec3 viewDir, Surface surf) {
    vec3 halfway = normalize(lightDir + viewDir);
    float nv = max(dot(norm, viewDir), 0.0);
    float nl = max(dot(norm, lightDir), 0.0);
    float nh = max(dot(norm, halfway), 0.0);
    float vh = max(dot(viewDir, halfway), 0.0);
    float lh = max(dot(lightDir, halfway), 0.0);
    float lum = luminance(surf.color);
    vec3 tint = lum > 0.0 ? surf.color / lum : vec3(1);
    vec3 spec0 = mix(surf.specular * 0.08 * mix(vec3(1), tint, surf.specular_tint), surf.color, surf.metallic);
    float fh = FresnelApprox(lh);
    float fl = FresnelApprox(nl);
    float fv = FresnelApprox(nv);
    float fss90 = lh * lh * surf.roughness;
    // diffuse
    float fd90 = 0.5 + 2.0 * fss90;
    float kd = mix(1.0, fd90, fl) * mix(1.0, fd90, fv);
    // vec3 diffuse = kd * surf.color / PI;
    // subsurface
    float fss = mix(1.0, fss90, fl) * mix(1.0, fss90, fv);
    float ss = 1.25 * (fss * (1.0 / (nl + nv + 0.0001) - 0.5) + 0.5);
    // specular
    float aspect = sqrt(1.0 - surf.anisotropic * .9);
    float ax = max(.001, surf.roughness * surf.roughness / aspect);
    float ay = max(.001, surf.roughness * surf.roughness * aspect);
    vec3 x = tangent;
    vec3 y = bitangent; //TODO: no recalculation
    float hx = dot(halfway, x);
    float hy = dot(halfway, y);
    float lx = dot(lightDir, x);
    float ly = dot(lightDir, y);
    float vx = dot(viewDir, x);
    float vy = dot(viewDir, y);
    float D = GTR2_aniso(nh, hx, hy, ax, ay); // TrowbridgeReitzGGX(nh, surf.roughness);
    float G = SmithGGX_aniso(nl, lx, ly, ax, ay) * SmithGGX_aniso(nv, vx, vy, ax, ay); // SmithsSchlickGGX(nv, nl, surf.roughness + 0.01) / max(4.0 * nv * nl, 0.0001);
    vec3 F = mix(spec0, vec3(1.0), fh);
    vec3 specular = D * G * F;
    // sheen
    vec3 sheen = fh * surf.sheen * mix(vec3(1), tint, surf.sheen_tint);
    // clearcoat
    float Dr = GTR1(nh, mix(.1, .001, surf.clearcoat_gloss));
    float Fr = mix(.04, 1.0, fh);
    float Gr = SmithGGX(nl, .25) * SmithGGX(nv, .25);
    vec3 clearcoat = 0.25 * vec3(surf.clearcoat) * Dr * Fr * Gr;

    return ((mix(kd, ss, surf.subsurface) * surf.color / PI + sheen) * (1.0 - surf.metallic) + specular + clearcoat) * nl;
}

void main() {
    vec4 baseColor = texture(base_map, tex_uv);
    if (baseColor.a < 0.5) {
        discard;
    }

    vec3 texNorm = texture(normal_map, tex_uv).rgb * 2.0 - 1.0;
    vec3 B = normalize(cross(N, T));
    mat3 TBN = mat3(T, B, N);
    vec3 norm = normalize(TBN * texNorm);
    vec3 tangent = normalize(T);
    vec3 bitangent = normalize(cross(norm, tangent));
    vec3 viewDir = normalize(camPos - frag_pos);

    Surface surf;
    surf.color = pow(baseColor.rgb, vec3(GAMMA));
    surf.ao = 1.0; // texture(ao_map, tex_uv).r;
    surf.roughness = 1.0; // texture(roughness_map, tex_uv).r;
    surf.metallic = 0.0; // texture(metallic_map, tex_uv).r;
    // surf.emission = vec3(0.0);
    surf.subsurface = 0.0;
    surf.specular = 0.5;
    surf.specular_tint = 0.0;
    surf.anisotropic = 0.0;
    surf.sheen = 0.0;
    surf.sheen_tint = 0.5;
    surf.clearcoat = 0.0;
    surf.clearcoat_gloss = 1.0;

    vec3 result = vec3(0.0);
    result += CalculateDirectionalLight(directional_lights[0], norm, tangent, bitangent, viewDir, surf);

    vec2 screenUV = gl_FragCoord.xy / screenSize;
    uint tileX = uint(screenUV.x * float(gridSize.x));
    uint tileY = uint((1.0 - screenUV.y) * float(gridSize.y));
    uint tileIndex = tileX + tileY * gridSize.x;
    Cluster tile = clusters[tileIndex];
    for (uint i = 0; i < tile.lightCount; i++) { // note that there is another light count variable
        uint lightIndex = tile.lightIndices[i];
        result += CalculatePointLight(point_lights[lightIndex], norm, tangent, bitangent, viewDir, surf);
    }

    result += vec3(0.2) * surf.ao * surf.color;
    // result += CalculateIBL(norm, viewDir, surf);

    Color = vec4(result, 1.0);
}
