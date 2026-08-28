#version 450
// Water planar reflection — fragment stage. GLSL twin of 3d_water_refl_rhi.metal.
//
// Simplified shading for the mirrored render: albedo x (IBL irradiance +
// unshadowed sun lambert) + emissive. Seen only through the rippled water
// surface, so skipping shadows/specular/clustered lights is invisible and
// keeps the pass one draw per mesh. The waterline clip (fragment discard,
// Atmospheric-style — portable, no gl_ClipDistance) keeps submerged geometry
// out of the mirror, with a fixed 0.08 m slack covering the ripple amplitude.

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragUV;
layout(location = 3) flat in uint fragMaterialID;

layout(location = 0) out vec4 outColor;

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
    vec4 sunDirection;        // xyz = world dir FROM the sun
    vec4 sunColorIntensity;   // rgb + w = intensity
    vec4 causticsParams;      // w = water level Y
    vec4 causticsBoundsMin;
    vec4 causticsBoundsMax;
    mat4 reflectionViewProj;
    vec4 fftParams;
    vec4 reflectionParams;
} water;

// Material array — layout must match Vapor::MaterialData (112 bytes).
struct MaterialData {
    vec4 baseColorFactor;
    float normalScale;
    float metallicFactor;
    float roughnessFactor;
    float occlusionStrength;
    vec3 emissiveFactor;
    float alphaCutoff;
    float emissiveStrength;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
    float prototypeUVMode;
    float uvScale;
    float iblEnabled;
    float transmission;
};
layout(std430, set = 0, binding = 1) readonly buffer MaterialBuf {
    MaterialData materials[];
};

layout(set = 2, binding = 0) uniform sampler2D albedoMap;      // per-draw
layout(set = 2, binding = 1) uniform samplerCube irradianceMap;

const float PI = 3.14159265359;

void main() {
    // Waterline clip: nothing below the surface belongs in the mirror.
    if (fragWorldPos.y < water.causticsParams.w - 0.08) {
        discard;
    }

    MaterialData mat = materials[fragMaterialID];
    vec4 albedoSample = texture(albedoMap, fragUV);
    if (albedoSample.a < mat.alphaCutoff) {
        discard;
    }
    // Albedo textures are authored sRGB; linearize like the main pass does.
    vec3 albedo = pow(albedoSample.rgb, vec3(2.2)) * mat.baseColorFactor.rgb;

    vec3 N = normalize(fragNormal);
    vec3 ambient = texture(irradianceMap, N).rgb;
    vec3 sunDir = -normalize(water.sunDirection.xyz);
    vec3 sun = water.sunColorIntensity.rgb * water.sunColorIntensity.w
             * max(dot(N, sunDir), 0.0) / PI;
    vec3 emissive = mat.emissiveFactor * mat.emissiveStrength;

    outColor = vec4(albedo * (ambient + sun) + emissive, 1.0);
}
