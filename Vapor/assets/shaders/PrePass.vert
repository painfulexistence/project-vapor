#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec3 tangent;

layout(location = 0) out vec2 tex_uv;

layout(push_constant) uniform PushConstants {
    uint instanceID;
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
    uint vertexOffset;
    uint indexOffset;
    uint vertexCount;
    uint indexCount;
    uint materialID;
    uint primitiveMode;
    vec3 boundingBoxMin;
    vec3 boundingBoxMax;
    vec4 boundingSphere; // x, y, z, radius
};
layout(set = 0, binding = 1) uniform InstanceBuffer {
    InstanceData instances[1000];
};

void main() {
    tex_uv = uv;
    mat4 model = instances[instanceID].model;
    vec3 frag_pos = vec3(model * vec4(position, 1.0));
    gl_Position = proj * view * vec4(frag_pos, 1.0);
}