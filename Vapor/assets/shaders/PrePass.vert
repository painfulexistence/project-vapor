#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec3 tangent;

layout(location = 0) out vec2 tex_uv;

layout(binding = 0) uniform CameraData {
    mat4 proj;
    mat4 view;
    mat4 invProj;
    float near;
    float far;
};
layout(binding = 1) uniform InstanceData {
    mat4 model;
    vec4 color;
};

void main() {
    tex_uv = uv;
    vec3 frag_pos = vec3(model * vec4(position, 1.0));
    gl_Position = proj * view * vec4(frag_pos, 1.0);
}