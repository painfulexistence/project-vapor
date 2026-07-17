#version 450
// RHI renderer main-pass vertex shader (Vulkan backend).
//
// Binding convention (see rhi_vulkan.cpp):
//   set 0 = vertex-stage buffers   (RHI::setVertexBuffer, SSBO std430)
//   set 1 = fragment-stage buffers (RHI::setFragmentBuffer)
//   set 2 = fragment textures      (RHI::setTexture)
//   push constants: vertex bytes at offset (binding%4)*16 in [0,64)

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3 fragPos;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec3 worldNormal;
layout(location = 3) out vec4 worldTangent;
layout(location = 4) flat out uint fragMaterialID;
layout(location = 5) out vec4 instanceColor;

// Must match CameraRenderData / CameraData (C++)
struct CameraData {
    mat4 proj;
    mat4 view;
    mat4 invProj;
    mat4 invView;
    float nearPlane;
    float farPlane;
    vec2 _pad;
    vec3 position;
    float _pad2;
    vec4 frustumPlanes[6];
};

// Must match Vapor::InstanceData (C++, std430 array stride = 160 bytes)
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

layout(std430, set = 0, binding = 0) readonly buffer CameraBuf {
    CameraData cam;
};
layout(std430, set = 0, binding = 2) readonly buffer InstanceBuf {
    InstanceData instances[];
};

// RHI::setVertexBytes(&instanceID, 4, /*binding=*/4) -> offset (4%4)*16 = 0
layout(push_constant) uniform PushConstants {
    uint instanceID;
};

void main() {
    // instanceID + gl_InstanceIndex: the normal/per-object paths pass the index
    // via the push constant and draw with firstInstance = 0, so gl_InstanceIndex
    // is 0 (a no-op). Single-call multi-draw indirect can't set a per-object
    // push constant, so it passes instanceID = 0 and carries the index in the
    // draw command's firstInstance, which surfaces as gl_InstanceIndex.
    InstanceData inst = instances[instanceID + gl_InstanceIndex];
    mat4 model = inst.model;

    mat3 normalMatrix = transpose(inverse(mat3(model)));
    worldNormal = normalMatrix * inNormal;
    worldTangent = vec4(normalMatrix * inTangent.xyz, inTangent.w);

    fragPos = vec3(model * vec4(inPosition, 1.0));
    fragUV = inUV;
    fragMaterialID = inst.materialID;
    instanceColor = inst.color;

    gl_Position = cam.proj * cam.view * vec4(fragPos, 1.0);
}
