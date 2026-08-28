#version 450
// Water planar reflection — vertex stage. GLSL twin of 3d_water_refl_rhi.metal.
//
// Renders every drawable through the MIRRORED camera (WaterData's
// reflectionViewProj: the main camera reflected about the water plane by a
// Householder matrix, Atmospheric-style). The mirrored view flips winding, so
// the pipeline culls FRONT faces; the fragment stage clips everything below
// the waterline so submerged geometry never leaks into the reflection.
//
//   set 0, binding 0 = WaterData (reflection matrix + waterline + sun)
//   set 0, binding 1 = material array (baseColorFactor / cutoff for MASK)
//   set 0, binding 2 = InstanceBuf
//   push: instanceID (offset 0)

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragUV;
layout(location = 3) flat out uint fragMaterialID;

struct Wave {
    vec3 direction;
    float _wpad;
    float steepness;
    float waveLength;
    float amplitude;
    float speed;
};

// Full CPU WaterData layout (graphics_effects.hpp) — keep in exact sync.
layout(std430, set = 0, binding = 0) readonly buffer WaterBuf {
    mat4 modelMatrix;
    vec4 surfaceColor;
    vec4 refractionColor;
    vec4 ssrSettings;
    vec4 normalMapScroll;
    vec2 normalMapScrollSpeed;
    vec2 _pad1;
    float refractionDistortionFactor;
    float refractionHeightFactor;
    float refractionDistanceFactor;
    float depthSofteningDistance;
    float foamHeightStart;
    float foamFadeDistance;
    float foamTiling;
    float foamAngleExponent;
    float roughness;
    float reflectance;
    float specIntensity;
    float foamBrightness;
    Wave waves[4];
    uint waveCount;
    float dampeningFactor;
    float time;
    float _pad2;
    vec4 sunDirection;
    vec4 sunColorIntensity;
    vec4 causticsParams;      // w = water level Y
    vec4 causticsBoundsMin;
    vec4 causticsBoundsMax;
    mat4 reflectionViewProj;
    vec4 fftParams;
    vec4 reflectionParams;
} water;

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

layout(std430, set = 0, binding = 2) readonly buffer InstanceBuf {
    InstanceData instances[];
};

layout(push_constant) uniform PushConstants {
    layout(offset = 0) uint instanceID;
};

void main() {
    InstanceData inst = instances[instanceID];
    vec4 world = inst.model * vec4(inPosition, 1.0);
    fragWorldPos = world.xyz;
    fragNormal = normalize(mat3(inst.model) * inNormal);
    fragUV = inUV;
    fragMaterialID = inst.materialID;
    gl_Position = water.reflectionViewProj * world;
}
