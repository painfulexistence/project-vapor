#version 450
// Depth-only shadow pass: no color output; depth is written by the fixed
// function. The only fragment work is the glTF MASK alpha cutout — MASK
// foliage must punch holes in the shadow map instead of casting solid quads.
// Materials without a cutoff (alphaCutoff == 0) never sample, so opaque
// geometry stays a pure depth write. NOTE: no early_fragment_tests here —
// discard requires late depth.

layout(location = 0) in vec2 fragUV;
layout(location = 1) flat in uint fragMaterialID;

// Must match Vapor::MaterialData (same block as RHIMain.frag / PrePass.frag).
struct MaterialData {
    vec4 baseColorFactor;
    float normalScale;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    vec3 emissiveFactor;
    float alphaCutoff;// MASK-mode cutoff; 0 = disabled
    float emissiveStrength;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
};
layout(std430, set = 0, binding = 1) readonly buffer MaterialBuf {
    MaterialData materials[];
};

layout(set = 2, binding = 0) uniform sampler2D albedoMap;

void main() {
    MaterialData mat = materials[fragMaterialID];
    if (mat.alphaCutoff > 0.0) {
        float a = texture(albedoMap, fragUV).a * mat.baseColorFactor.a;
        if (a < mat.alphaCutoff) discard;
    }
}
