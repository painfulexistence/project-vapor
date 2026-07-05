#version 450
// Directional shadow depth pass: transform instance geometry into light space.
// Reuses the main pass's InstanceBuf (set 0, binding 2) and the per-draw
// instanceID push constant; the light-space matrix lives in set 0, binding 0.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec4 inTangent;

struct InstanceData {
    mat4 model;
    vec4 color;
    uint vertexOffset;
    uint indexOffset;
    uint vertexCount;
    uint indexCount;
    uint materialID;
    uint primitiveMode;
    uvec2 _pad1;
    vec3 aabbMin;
    float _pad2;
    vec3 aabbMax;
    float _pad3;
    vec4 boundingSphere;
};

layout(std430, set = 0, binding = 0) readonly buffer LightBuf {
    mat4 lightSpaceMatrix;
};
layout(std430, set = 0, binding = 2) readonly buffer InstanceBuf {
    InstanceData instances[];
};

layout(push_constant) uniform PushConstants {
    uint instanceID;
};

void main() {
    InstanceData inst = instances[instanceID];
    vec3 worldPos = vec3(inst.model * vec4(inPosition, 1.0));
    gl_Position = lightSpaceMatrix * vec4(worldPos, 1.0);
}
