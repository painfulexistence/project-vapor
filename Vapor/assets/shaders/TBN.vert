#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec3 tangent;

layout(location = 0) out vec3 frag_pos;
layout(location = 1) out vec2 tex_uv;
layout(location = 2) out vec3 T;
layout(location = 3) out vec3 N;

layout(binding = 0) uniform UBO {
    mat4 model;
    mat4 view;
    mat4 proj;
};

void main() {
    T = normalize(vec3(model * vec4(tangent, 0.0)));
    N = normalize(vec3(model * vec4(normal, 0.0)));

    frag_pos = vec3(model * vec4(position, 1.0));
    tex_uv = uv;

    gl_Position = proj * view * vec4(frag_pos, 1.0);
}