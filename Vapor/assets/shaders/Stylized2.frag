#version 450

layout(location = 0) in vec3 frag_pos;
layout(location = 1) in vec2 tex_uv;
layout(location = 2) in vec3 world_normal;
layout(location = 3) in vec4 world_tangent;
layout(location = 0) out vec4 Color;

layout(set = 1, binding = 0) uniform sampler2D base_map;
layout(set = 1, binding = 1) uniform sampler2D normal_map;
layout(set = 1, binding = 2) uniform sampler2D metallic_roughness_map;
layout(set = 1, binding = 3) uniform sampler2D occlusion_map;
layout(set = 1, binding = 4) uniform sampler2D emission_map;

void main() {
    vec4 baseColor = texture(base_map, tex_uv);
    if (baseColor.a < 0.5) {
        discard;
    }

    vec3 N = normalize(world_normal);
    vec3 T = normalize(world_tangent.xyz);
    T = normalize(T - dot(T, N) * N);
    vec3 B = normalize(cross(N, T) * world_tangent.w);
    mat3 TBN = mat3(T, B, N);
    vec3 norm = world_normal;

    vec3 color = N * 0.5 + 0.5;
    float len = length(color);
    color = (len > 0.9 && len < 1.1) ? color / len : vec3(0.0);

    Color = vec4(color, 1.0);
}