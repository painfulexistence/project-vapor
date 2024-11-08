#version 450

layout(location = 0) in vec2 tex_uv;
layout(location = 1) in vec3 frag_pos;
layout(location = 2) in vec3 frag_normal;
layout(location = 0) out vec4 Color;
layout(binding = 1) uniform sampler2D base_map;
layout(binding = 2) uniform sampler2D normal_map;
layout(push_constant) uniform PushConstantBlock {
    vec3 cam_pos;
    float time;
};

struct Surface {
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
    float shininess;
};

struct DirLight {
    vec3 direction;
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
    float intensity;
};

struct PointLight {
    vec3 position;
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
    float intensity;
};

DirLight main_light = DirLight(vec3(0.0f, 1.0f, 0.0f), vec3(0.2f), vec3(1.0f), vec3(0.8f), 1.0f);
Surface surf = Surface(vec3(0.5f), vec3(1.0f, 1.0f, 0.8f), vec3(0.5f), 2.0f);

void main() {
	vec3 norm = normalize(frag_normal);
    vec3 viewDir = normalize(cam_pos - frag_pos);
    vec3 lightDir = normalize(-main_light.direction);
    vec3 halfway = normalize(lightDir + viewDir);

    float diff = max(dot(norm, lightDir), 0.0);
    vec3 surfColor = texture(base_map, tex_uv).xyz;

    vec3 ambient = main_light.ambient * main_light.intensity * surf.ambient;
    vec3 diffuse = main_light.diffuse * main_light.intensity * (diff * surfColor);
    vec3 specular = main_light.specular * main_light.intensity * pow(max(dot(halfway, norm), 0.0), surf.shininess) * surf.specular;

    vec3 result = ambient + diffuse + specular;

    Color = vec4(result, 1.0);
}