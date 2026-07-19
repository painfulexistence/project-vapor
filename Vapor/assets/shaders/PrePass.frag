#version 450
// Depth + normal + albedo pre-pass (RHI renderer, Vulkan backend). Consumes a
// subset of RHIMain.vert's outputs (matching locations). Output 0 = world
// normal (RGBA16F normalRT), output 1 = base color (RGBA8 albedoRT) — the
// inputs future SSAO/GIBS-style passes need; depth is written by the pipeline.

layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 worldNormal;
layout(location = 4) flat in uint fragMaterialID;

layout(location = 0) out vec4 outNormal;
layout(location = 1) out vec4 outAlbedo;

// Must match Vapor::MaterialData (same block as RHIMain.frag).
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
    // Tail kept layout-matched with the C++/MSL twins (unused here).
    float prototypeUVMode;
    float uvScale;
    float iblEnabled;
    float transmission;
};
layout(std430, set = 0, binding = 1) readonly buffer MaterialBuf {
    MaterialData materials[];
};

layout(set = 2, binding = 0) uniform sampler2D albedoMap;

void main() {
    MaterialData mat = materials[fragMaterialID];
    vec4 baseSample = texture(albedoMap, fragUV);
    // Alpha cutout: keep the depth buffer's holes in sync with RHIMain.frag —
    // a solid depth quad here would z-reject the background behind the holes.
    if (mat.alphaCutoff > 0.0 && baseSample.a * mat.baseColorFactor.a < mat.alphaCutoff) discard;
    outNormal = vec4(normalize(worldNormal), 1.0);
    outAlbedo = vec4(baseSample.rgb * mat.baseColorFactor.rgb, 1.0);
}
