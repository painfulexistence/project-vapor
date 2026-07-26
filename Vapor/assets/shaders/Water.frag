#version 450
// RHI water surface — fragment stage. GLSL twin of 3d_water_rhi.metal.
//
// Port of the legacy Metal-native water (3d_water.metal) onto the RHI frame:
// scrolling dual normal maps over the Gerstner surface, GGX sun specular with
// sparkle noise, screen-space reflections raymarched in view space against the
// scene depth/normals, refraction from a pre-water snapshot of the HDR scene
// (tempColorRT copy — reading the live color target is invalid), depth-based
// tint + edge softening, and height/angle/shore foam.
//
// Two deliberate upgrades over the legacy shader:
//  * The environment fallback actually samples the IBL prefiltered cubemap
//    (the legacy shader hardcoded an outdoor sky gradient — wrong anywhere,
//    fatally wrong indoors). causticsBoundsMax.w scales it.
//  * The sun comes from WaterData (mirrored from the atmosphere by the pass,
//    like the fog passes do) instead of a directional-light buffer binding.

layout(location = 0) in vec3 vNormalView;
layout(location = 1) in vec3 vTangentView;
layout(location = 2) in vec3 vBinormalView;
layout(location = 3) in vec4 vPositionView;
layout(location = 4) in vec4 vTexCoord0;
layout(location = 5) in vec4 vScreenPosition;
layout(location = 6) in vec4 vPositionWorld;
layout(location = 7) in vec4 vWorldNormalAndHeight;

layout(location = 0) out vec4 outColor;

struct Wave {
    vec3 direction;
    float _wpad;   // std430 packs the next float into a vec3's tail; the CPU
                   // WaveData has an explicit pad here, so mirror it
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
    vec4 ssrSettings;          // x=step size y=max steps (0=off) z=refine steps w=depth factor
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

layout(set = 2, binding = 0) uniform sampler2D waterNormalMap1;
layout(set = 2, binding = 1) uniform sampler2D waterNormalMap2;
layout(set = 2, binding = 2) uniform sampler2D sceneColor;   // pre-water snapshot
layout(set = 2, binding = 3) uniform sampler2D sceneDepth;
layout(set = 2, binding = 4) uniform sampler2D sceneNormal;  // world-space normals
layout(set = 2, binding = 5) uniform samplerCube envMap;     // IBL prefiltered cube
layout(set = 2, binding = 6) uniform sampler2D foamMap;
layout(set = 2, binding = 7) uniform sampler2D noiseMap;

const float PI = 3.14159265359;
const float EPSILON = 0.0001;

// ── PBR helpers (same forms as the legacy shader) ───────────────────────────

float calculateNormalDistributionGGX(float linearRoughness, float nDotH) {
    float a2 = linearRoughness * linearRoughness;
    float d = (nDotH * nDotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

vec3 calculateSchlickFresnelReflectance(float lDotH, vec3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - lDotH, 5.0);
}

float calculateSmithGGXGeometryTerm(float linearRoughness, float nDotL, float nDotV) {
    float k = linearRoughness * 0.5;
    float ggxL = nDotL / (nDotL * (1.0 - k) + k);
    float ggxV = nDotV / (nDotV * (1.0 - k) + k);
    return ggxL * ggxV;
}

// ── Position reconstruction (engine convention: y-down UV, [0,1] depth) ─────

vec2 clipToScreenUV(vec4 clipPos) {
    vec2 ndc = clipPos.xy / clipPos.w;
    return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

vec3 getWorldPositionFromDepth(vec2 uv, float depth) {
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    vec4 viewPos = invProj * vec4(ndc, depth, 1.0);
    viewPos /= viewPos.w;
    return (invView * vec4(viewPos.xyz, 1.0)).xyz;
}

vec3 getViewPositionFromDepth(vec2 uv, float depth) {
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    vec4 viewPos = invProj * vec4(ndc, depth, 1.0);
    return viewPos.xyz / viewPos.w;
}

void main() {
    vec3 normal = normalize(vNormalView);
    vec3 tangent = normalize(vTangentView);
    vec3 binormal = normalize(vBinormalView);

    vec2 normalMapCoords1 = vTexCoord0.xy + water.time * water.normalMapScroll.xy * water.normalMapScrollSpeed.x;
    vec2 normalMapCoords2 = vTexCoord0.xy + water.time * water.normalMapScroll.zw * water.normalMapScrollSpeed.y;

    vec2 hdrCoords = clipToScreenUV(vScreenPosition);

    // Blend the two scrolling normal maps in view space.
    vec3 normalMap1 = texture(waterNormalMap1, normalMapCoords1).rgb * 2.0 - 1.0;
    vec3 normalMap2 = texture(waterNormalMap2, normalMapCoords2).rgb * 2.0 - 1.0;
    mat3 texSpace = mat3(tangent, binormal, normal);
    vec3 finalNormal = normalize(texSpace * normalMap1);
    finalNormal += normalize(texSpace * normalMap2);
    finalNormal = normalize(finalNormal);

    // ── Sun specular (GGX + sparkle noise) ──────────────────────────────────
    float linearRoughness = water.roughness * water.roughness;
    vec3 viewDir = -normalize(vPositionView.xyz);
    vec3 lightDir = -normalize((view * vec4(water.sunDirection.xyz, 0.0)).xyz);
    vec3 halfVec = normalize(viewDir + lightDir);
    float nDotL = clamp(dot(finalNormal, lightDir), 0.0, 1.0);
    float nDotV = abs(dot(finalNormal, viewDir)) + EPSILON;
    float nDotH = clamp(dot(finalNormal, halfVec), 0.0, 1.0);
    float lDotH = clamp(dot(lightDir, halfVec), 0.0, 1.0);

    vec3 f0 = vec3(0.16 * water.reflectance * water.reflectance);
    float normalDistribution = calculateNormalDistributionGGX(linearRoughness, nDotH);
    vec3 fresnelReflectance = calculateSchlickFresnelReflectance(lDotH, f0);
    float geometryTerm = calculateSmithGGXGeometryTerm(linearRoughness, nDotL, nDotV);

    float specularNoise = texture(noiseMap, normalMapCoords1 * 0.5).r;
    specularNoise *= texture(noiseMap, normalMapCoords2 * 0.5).r;
    specularNoise *= texture(noiseMap, vTexCoord0.xy * 0.5).r;

    vec3 specularFactor = (geometryTerm * normalDistribution) * fresnelReflectance
                        * water.specIntensity * nDotL * specularNoise;

    // ── Screen-space reflections ────────────────────────────────────────────
    vec3 reflectionVector = normalize(reflect(-viewDir, finalNormal));
    bool ssrEnabled = water.ssrSettings.y > 0.0;

    vec3 rayMarchPosition = vPositionView.xyz;
    vec2 hitUV = vec2(0.0);
    float stepCount = 0.0;
    float forwardStepCount = water.ssrSettings.y;
    vec3 finalSceneViewPos = vec3(0.0);
    bool foundHit = false;

    if (ssrEnabled) {
        while (stepCount < water.ssrSettings.y) {
            rayMarchPosition += reflectionVector * water.ssrSettings.x;

            vec4 rayClip = proj * vec4(rayMarchPosition, 1.0);
            if (abs(rayClip.w) < EPSILON) rayClip.w = EPSILON;

            // Outside the frustum: keep marching (the ray may re-enter).
            if (abs(rayClip.x) > rayClip.w || abs(rayClip.y) > rayClip.w || rayClip.z > rayClip.w) {
                stepCount += 1.0;
                continue;
            }

            vec2 rayUV = clipToScreenUV(rayClip);
            if (rayUV.x < 0.0 || rayUV.x > 1.0 || rayUV.y < 0.0 || rayUV.y > 1.0) {
                stepCount += 1.0;
                continue;
            }

            float sceneZ = texture(sceneDepth, rayUV).r;
            vec3 sceneViewPos = getViewPositionFromDepth(rayUV, sceneZ);

            // View-space Z is negative into the screen: the scene being
            // "in front of" the ray point means the ray passed behind it.
            if (sceneViewPos.z >= rayMarchPosition.z) {
                forwardStepCount = stepCount;
                finalSceneViewPos = sceneViewPos;
                hitUV = rayUV;
                foundHit = true;
                break;
            }
            stepCount += 1.0;
        }

        // Backwards refinement toward the exact intersection.
        if (foundHit && forwardStepCount < water.ssrSettings.y) {
            float refineStep = 0.0;
            while (refineStep < water.ssrSettings.z) {
                rayMarchPosition -= reflectionVector * water.ssrSettings.x / water.ssrSettings.z;
                vec4 rayClip = proj * vec4(rayMarchPosition, 1.0);
                if (abs(rayClip.w) < EPSILON) rayClip.w = EPSILON;
                vec2 rayUV = clamp(clipToScreenUV(rayClip), vec2(0.0), vec2(1.0));

                float sceneZ = texture(sceneDepth, rayUV).r;
                vec3 sceneViewPos = getViewPositionFromDepth(rayUV, sceneZ);

                if (sceneViewPos.z < rayMarchPosition.z) {
                    break;  // stepped back out in front of the surface
                }
                hitUV = rayUV;
                finalSceneViewPos = sceneViewPos;
                refineStep += 1.0;
            }
        }
    }

    float ssrFactor = 0.0;
    if (ssrEnabled && foundHit) {
        vec3 ssrReflectionNormal = normalize(texture(sceneNormal, hitUV).xyz);
        vec3 ssrReflectionNormalView = normalize((view * vec4(ssrReflectionNormal, 0.0)).xyz);
        vec2 ssrDistanceFactor = vec2(abs(0.5 - hdrCoords.x), abs(0.5 - hdrCoords.y)) * 2.0;

        float hitDistanceFactor = (forwardStepCount < water.ssrSettings.y)
            ? (1.0 - forwardStepCount / water.ssrSettings.y) : 0.0;
        float depthFactor = 1.0 / (1.0 + abs(finalSceneViewPos.z - rayMarchPosition.z) * water.ssrSettings.w);
        float normalFactor = 1.0 - clamp(dot(ssrReflectionNormalView, finalNormal), 0.0, 1.0);

        ssrFactor = (1.0 - abs(nDotV))
                  * hitDistanceFactor
                  * clamp(1.0 - ssrDistanceFactor.x - ssrDistanceFactor.y, 0.0, 1.0)
                  * depthFactor
                  * normalFactor;
    }

    vec3 ssrColor = (foundHit && ssrFactor > 0.001) ? texture(sceneColor, hitUV).rgb : vec3(0.0);

    // Environment fallback: the IBL prefiltered cubemap by world reflection
    // direction, roughness-matched LOD (split-sum convention of RHIMain.frag).
    vec3 envReflectionDir = (invView * vec4(reflectionVector, 0.0)).xyz;
    vec3 envColor = textureLod(envMap, envReflectionDir, water.roughness * 4.0).rgb
                  * water.causticsBoundsMax.w;

    vec3 reflectionColor = mix(envColor, ssrColor, clamp(ssrFactor, 0.0, 1.0)) * water.surfaceColor.rgb;

    // ── Refraction ──────────────────────────────────────────────────────────
    vec2 distortedTexCoord = hdrCoords + ((finalNormal.xz + finalNormal.xy) * 0.5) * water.refractionDistortionFactor;
    float distortedDepth = texture(sceneDepth, distortedTexCoord).r;
    vec3 distortedPosition = getWorldPositionFromDepth(distortedTexCoord, distortedDepth);

    // Reject distorted samples that land on geometry above the surface.
    vec2 refractionTexCoord = (distortedPosition.y < vPositionWorld.y) ? distortedTexCoord : hdrCoords;
    vec3 waterColor = texture(sceneColor, refractionTexCoord).rgb * water.refractionColor.rgb;

    float sceneDepthSample = texture(sceneDepth, hdrCoords).r;
    vec3 scenePosition = getWorldPositionFromDepth(hdrCoords, sceneDepthSample);

    float depthSoftenedAlpha = clamp(length(scenePosition - vPositionWorld.xyz) / water.depthSofteningDistance, 0.0, 1.0);

    vec3 waterSurfacePosition = (distortedPosition.y < vPositionWorld.y) ? distortedPosition : scenePosition;
    waterColor = mix(waterColor, water.refractionColor.rgb,
                     clamp((vPositionWorld.y - waterSurfacePosition.y) / water.refractionHeightFactor, 0.0, 1.0));

    // ── Combine ─────────────────────────────────────────────────────────────
    vec3 geometricNormal = normalize(vNormalView);
    float waveTopReflectionFactor = pow(1.0 - clamp(dot(geometricNormal, viewDir), 0.0, 1.0), 3.0);
    float reflectionBlend = clamp(clamp(length(vPositionView.xyz) / water.refractionDistanceFactor, 0.0, 1.0)
                                  + waveTopReflectionFactor, 0.0, 1.0);
    vec3 waterBaseColor = mix(waterColor, reflectionColor, reflectionBlend);

    vec3 finalWaterColor = waterBaseColor
                         + specularFactor * water.sunColorIntensity.rgb * water.sunColorIntensity.w;

    // ── Foam ────────────────────────────────────────────────────────────────
    vec3 foamColor = texture(foamMap, (normalMapCoords1 + normalMapCoords2) * water.foamTiling).rgb;
    float foamNoise = texture(noiseMap, vTexCoord0.xy * water.foamTiling).r;

    float foamAmount = clamp((vWorldNormalAndHeight.w - water.foamHeightStart) / water.foamFadeDistance, 0.0, 1.0);
    foamAmount *= pow(clamp(dot(vWorldNormalAndHeight.xyz, vec3(0.0, 1.0, 0.0)), 0.0, 1.0), water.foamAngleExponent);
    foamAmount *= foamNoise;
    foamAmount += pow(1.0 - depthSoftenedAlpha, 3.0);

    finalWaterColor = mix(finalWaterColor, foamColor * water.foamBrightness,
                          clamp(foamAmount, 0.0, 1.0) * depthSoftenedAlpha);

    outColor = vec4(finalWaterColor, depthSoftenedAlpha);
}
