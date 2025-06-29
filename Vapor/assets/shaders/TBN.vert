#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec4 tangent;

layout(location = 0) out vec3 frag_pos;
layout(location = 1) out vec2 tex_uv;
layout(location = 2) out vec3 world_normal;
layout(location = 3) out vec4 world_tangent;

layout(push_constant) uniform PushConstants {
    layout(offset = 12) uint instanceID;
};
layout(binding = 0) uniform CameraData {
    mat4 proj;
    mat4 view;
    mat4 invProj;
    mat4 invView;
    float near;
    float far;
};
struct InstanceData {
    mat4 model;
    vec4 color;
};
layout(set = 0, binding = 1) uniform InstanceBuffer {
    InstanceData instances[1000];
};

void main() {
    mat4 model = instances[instanceID].model;
    // Caution: world normal and tangent are not normalized here
    mat3 normalMatrix = transpose(inverse(mat3(model)));
    world_normal = vec3(normalMatrix * normal);
    world_tangent = vec4(vec3(normalMatrix * tangent.xyz), tangent.w);

    frag_pos = vec3(model * vec4(position, 1.0));
    tex_uv = uv;

    gl_Position = proj * view * vec4(frag_pos, 1.0);
}