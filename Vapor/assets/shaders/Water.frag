#version 450
// RHI water surface — fragment stage. GLSL twin of 3d_water_rhi.metal.
//
// Shading over the FFT-simulated surface:
//  * Normals from the simulation's normal/foam map plus two scrolling detail
//    normal layers for close-up sparkle.
//  * Reflections from the planar-reflection pass (mirrored camera render),
//    distorted by the surface normal, falling back to the IBL prefiltered
//    cubemap outside the reflection RT or when planar is disabled.
//  * Refraction from the pre-water scene snapshot with depth-based tinting.
//  * Physically-driven Fresnel blend between the two, GGX sun specular,
//    Jacobian whitecap foam + shoreline foam, soft edges where the surface
//    meets geometry.

layout(location = 0) in vec4 vPositionView;
layout(location = 1) in vec4 vTexCoord0;       // xy: patch UV, zw: grid UV
layout(location = 2) in vec4 vScreenPosition;
layout(location = 3) in vec4 vPositionWorld;   // w: edge dampening
layout(location = 4) in vec4 vDispSample;

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
layout(std430, set = 1, binding = 0) readonly buffer WaterBuf {
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
    vec4 causticsParams;
    vec4 causticsBoundsMin;
    vec4 causticsBoundsMax;
    mat4 reflectionViewProj;
    vec4 fftParams;         // x=patch size y=detail tiling z=detail strength w=displacement scale
    vec4 reflectionParams;  // x=planar strength y=distortion z=whitecap scale w=FFT resolution
} water;

layout(std430, set = 1, binding = 1) readonly buffer CameraData {
    mat4 proj;
    mat4 view;
    mat4 invProj;
    mat4 invView;
    float near;
    float far;
    vec3 cameraPosition;
    float _camPad;
};

layout(set = 2, binding = 0) uniform sampler2D simNormalFoam;   // FFT normal + whitecap
layout(set = 2, binding = 1) uniform sampler2D detailNormal1;
layout(set = 2, binding = 2) uniform sampler2D detailNormal2;
layout(set = 2, binding = 3) uniform sampler2D sceneColor;      // pre-water snapshot
layout(set = 2, binding = 4) uniform sampler2D sceneDepth;
layout(set = 2, binding = 5) uniform samplerCube envMap;        // IBL prefiltered cube
layout(set = 2, binding = 6) uniform sampler2D reflectionMap;   // planar reflection RT
layout(set = 2, binding = 7) uniform sampler2D foamMap;
layout(set = 2, binding = 8) uniform sampler2D noiseMap;

const float PI = 3.14159265359;
const float EPSILON = 0.0001;

float ndfGGX(float linearRoughness, float nDotH) {
    float a2 = linearRoughness * linearRoughness;
    float d = (nDotH * nDotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float smithGGX(float linearRoughness, float nDotL, float nDotV) {
    float k = linearRoughness * 0.5;
    return (nDotL / (nDotL * (1.0 - k) + k)) * (nDotV / (nDotV * (1.0 - k) + k));
}

vec2 clipToScreenUV(vec4 clipPos) {
    vec2 ndc = clipPos.xy / clipPos.w;
    return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

vec3 worldPosFromDepth(vec2 uv, float depth) {
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    vec4 viewPos = invProj * vec4(ndc, depth, 1.0);
    viewPos /= viewPos.w;
    return (invView * vec4(viewPos.xyz, 1.0)).xyz;
}

void main() {
    float dampening = clamp(vPositionWorld.w, 0.0, 1.0);
    vec2 patchUV = vTexCoord0.xy;

    // ── Surface normal: FFT sim + scrolling detail layers ───────────────────
    vec4 simSample = texture(simNormalFoam, patchUV);
    vec3 N = normalize(simSample.xyz);
    // Flatten toward +Y at the pool edges together with the displacement.
    N = normalize(mix(vec3(0.0, 1.0, 0.0), N, dampening));

    vec2 det1UV = patchUV * water.fftParams.y + water.time * water.normalMapScroll.xy * water.normalMapScrollSpeed.x;
    vec2 det2UV = patchUV * water.fftParams.y + water.time * water.normalMapScroll.zw * water.normalMapScrollSpeed.y;
    vec3 dn1 = texture(detailNormal1, det1UV).rgb * 2.0 - 1.0;
    vec3 dn2 = texture(detailNormal2, det2UV).rgb * 2.0 - 1.0;
    // Detail maps are +Z-up tangent space over a horizontal surface: xy -> xz.
    vec2 detXZ = (dn1.xy + dn2.xy) * (0.5 * water.fftParams.z * dampening);
    N = normalize(vec3(N.x + detXZ.x, N.y, N.z + detXZ.y));

    vec3 viewDir = -normalize(vPositionView.xyz);
    vec3 viewDirWorld = normalize(cameraPosition - vPositionWorld.xyz);
    vec2 hdrCoords = clipToScreenUV(vScreenPosition);

    // ── Fresnel ─────────────────────────────────────────────────────────────
    float f0 = 0.16 * water.reflectance * water.reflectance;
    float nDotVWorld = clamp(dot(N, viewDirWorld), 0.0, 1.0);
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - nDotVWorld, 5.0);

    // ── Reflection: planar RT with normal distortion, env cube fallback ─────
    vec3 envDir = reflect(-viewDirWorld, N);
    vec3 envColor = textureLod(envMap, envDir, water.roughness * 4.0).rgb
                  * water.causticsBoundsMax.w;

    vec3 reflectionColor = envColor;
    if (water.reflectionParams.x > 0.0) {
        vec4 rclip = water.reflectionViewProj * vec4(vPositionWorld.xyz, 1.0);
        if (rclip.w > EPSILON) {
            vec2 ruv = clipToScreenUV(rclip) + N.xz * water.reflectionParams.y;
            // Fade to the env cube toward the RT border instead of clamping.
            vec2 border = min(ruv, 1.0 - ruv);
            float inRT = clamp(min(border.x, border.y) * 12.0, 0.0, 1.0);
            vec3 planar = texture(reflectionMap, clamp(ruv, vec2(0.001), vec2(0.999))).rgb;
            reflectionColor = mix(envColor, planar, inRT * water.reflectionParams.x);
        }
    }
    reflectionColor *= water.surfaceColor.rgb;

    // ── Refraction: scene snapshot with depth-based absorption ──────────────
    vec2 distortedUV = hdrCoords + N.xz * water.refractionDistortionFactor;
    float distortedDepth = texture(sceneDepth, distortedUV).r;
    vec3 distortedPos = worldPosFromDepth(distortedUV, distortedDepth);
    vec2 refractionUV = (distortedPos.y < vPositionWorld.y) ? distortedUV : hdrCoords;
    vec3 refractionColor = texture(sceneColor, refractionUV).rgb * water.refractionColor.rgb;

    float sceneDepthHere = texture(sceneDepth, hdrCoords).r;
    vec3 scenePos = worldPosFromDepth(hdrCoords, sceneDepthHere);
    float depthSoftenedAlpha = clamp(length(scenePos - vPositionWorld.xyz) / water.depthSofteningDistance, 0.0, 1.0);

    vec3 refractedFloorPos = (distortedPos.y < vPositionWorld.y) ? distortedPos : scenePos;
    refractionColor = mix(refractionColor, water.refractionColor.rgb,
                          clamp((vPositionWorld.y - refractedFloorPos.y) / water.refractionHeightFactor, 0.0, 1.0));

    // ── Sun specular (GGX) ──────────────────────────────────────────────────
    vec3 lightDirWorld = -normalize(water.sunDirection.xyz);
    vec3 halfVec = normalize(viewDirWorld + lightDirWorld);
    float nDotL = clamp(dot(N, lightDirWorld), 0.0, 1.0);
    float nDotV = abs(dot(N, viewDirWorld)) + EPSILON;
    float nDotH = clamp(dot(N, halfVec), 0.0, 1.0);
    float linearRoughness = max(water.roughness * water.roughness, 1e-4);
    float specularNoise = texture(noiseMap, det1UV * 0.5).r * texture(noiseMap, det2UV * 0.5).r;
    vec3 specular = vec3(ndfGGX(linearRoughness, nDotH) * smithGGX(linearRoughness, nDotL, nDotV))
                  * fresnel * water.specIntensity * nDotL * (0.5 + specularNoise)
                  * water.sunColorIntensity.rgb * water.sunColorIntensity.w;

    // ── Combine ─────────────────────────────────────────────────────────────
    vec3 color = mix(refractionColor, reflectionColor, clamp(fresnel, 0.0, 1.0)) + specular;

    // ── Foam: whitecaps (displacement Jacobian) + shoreline ─────────────────
    float whitecap = simSample.a * water.reflectionParams.z;
    float shore = pow(1.0 - depthSoftenedAlpha, 3.0);
    float foamAmount = clamp(whitecap + shore, 0.0, 1.0)
                     * texture(noiseMap, patchUV * water.foamTiling).r;
    vec3 foamColor = texture(foamMap, patchUV * water.foamTiling + N.xz * 0.1).rgb * water.foamBrightness;
    color = mix(color, foamColor, clamp(foamAmount, 0.0, 1.0) * depthSoftenedAlpha);

    outColor = vec4(color, depthSoftenedAlpha);
}
