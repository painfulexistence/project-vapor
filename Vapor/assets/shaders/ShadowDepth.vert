#version 450
// Directional shadow depth pass (cascaded / PSSM): transform instance geometry
// into the light space of one cascade. All three cascade matrices are uploaded
// once in PSSMBuf; the cascade being rendered is selected by a push constant so
// a single buffer upload feeds every cascade pass (a per-cascade buffer rewrite
// would race, since host-visible updates are immediate and the GPU runs later).
//
//   set 0, binding 0 = PSSM data (light-space matrices + splits)  [vertex SSBO 0]
//   set 0, binding 2 = InstanceBuf                                [vertex SSBO 2]
//   push: instanceID (offset 0), cascadeIndex (offset 16)

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec4 inTangent;

// Passed to the fragment stage for the alpha-cutout test (MASK foliage must
// punch holes in the shadow map, not cast solid quads).
layout(location = 0) out vec2 fragUV;
layout(location = 1) flat out uint fragMaterialID;

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

// Must match Vapor::PSSMRenderData (std430).
layout(std430, set = 0, binding = 0) readonly buffer PSSMBuf {
    mat4 lightSpaceMatrices[3];
    vec4 cascadeSplits;
    float blendRange;          // 208
    float cascadeBlendRange;   // 212
    uint  pcfSampleCount;      // 216
    uint  debugVisualize;      // 220
    float nearShadowEnd;       // 224
    float _pad0;               // 228
    float _pad1;               // 232
    float _pad2;               // 236
    mat4 nearLightMatrix;      // 240  near-field map (cascadeIndex == 3)
};
layout(std430, set = 0, binding = 2) readonly buffer InstanceBuf {
    InstanceData instances[];
};

layout(push_constant) uniform PushConstants {
    layout(offset = 0)  uint instanceID;
    layout(offset = 16) uint cascadeIndex;
};

void main() {
    InstanceData inst = instances[instanceID];
    fragUV = inUV;
    fragMaterialID = inst.materialID;
    vec3 worldPos = vec3(inst.model * vec4(inPosition, 1.0));
    mat4 m = cascadeIndex < 3u ? lightSpaceMatrices[cascadeIndex] : nearLightMatrix;
    gl_Position = m * vec4(worldPos, 1.0);
}
