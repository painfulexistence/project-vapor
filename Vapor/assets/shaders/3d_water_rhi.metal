#include <metal_stdlib>
using namespace metal;

// RHI water surface — MSL twin of Water.vert / Water.frag. See those files for
// the design notes; the algorithm and the WaterData layout are identical (the
// CPU struct in graphics_effects.hpp is the single source of truth).
//
// Bindings through the RHI:
//   vertex:   buffer(0) = CameraRenderData, buffer(1) = WaterData,
//             buffer(2) = WaterVertexData[] (pulled by vertex_id)
//   fragment: buffer(0) = WaterData, buffer(1) = CameraRenderData,
//             texture(0..7) = normal1, normal2, sceneColor snapshot,
//             sceneDepth, sceneNormal, envCube, foam, noise; sampler(0..7).

struct CameraDataRHI {
    float4x4 proj;
    float4x4 view;
    float4x4 invProj;
    float4x4 invView;
    float nearPlane;
    float farPlane;
    float2 _pad;
    float3 position;
    float _pad2;
    // frustumPlanes follow on the CPU side; unused here.
};

struct WaveRHI {
    float3 direction;   // MSL float3 occupies 16 bytes, matching the CPU pad
    float steepness;
    float waveLength;
    float amplitude;
    float speed;
};

struct WaterDataRHI {
    float4x4 modelMatrix;
    float4 surfaceColor;
    float4 refractionColor;
    float4 ssrSettings;
    float4 normalMapScroll;
    float2 normalMapScrollSpeed;
    float2 _pad1;
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
    WaveRHI waves[4];
    uint waveCount;
    float dampeningFactor;
    float time;
    float _pad2;
    float4 sunDirection;
    float4 sunColorIntensity;
    float4 causticsParams;
    float4 causticsBoundsMin;
    float4 causticsBoundsMax;
};

struct WaterVertexRHI {
    packed_float3 position;
    packed_float2 uv0;
    packed_float2 uv1;
};

struct WaterVSOut {
    float4 position [[position]];
    float3 normalView;
    float3 tangentView;
    float3 binormalView;
    float4 positionView;
    float4 texCoord0;
    float4 screenPosition;
    float4 positionWorld;
    float4 worldNormalAndHeight;
};

struct WaveResultRHI {
    float3 position;
    float3 normal;
    float3 binormal;
    float3 tangent;
};

static WaveResultRHI calculateWaveRHI(WaveRHI wave, float3 wavePosition, float edgeDampen, float time, uint numWaves) {
    WaveResultRHI result;

    float frequency = 2.0 / wave.waveLength;
    float phaseConstant = wave.speed * frequency;
    float qi = wave.steepness / (wave.amplitude * frequency * float(numWaves));
    float rad = frequency * dot(float3(wave.direction).xz, wavePosition.xz) + time * phaseConstant;
    float sinR = sin(rad);
    float cosR = cos(rad);

    result.position.x = wavePosition.x + qi * wave.amplitude * wave.direction.x * cosR * edgeDampen;
    result.position.z = wavePosition.z + qi * wave.amplitude * wave.direction.z * cosR * edgeDampen;
    result.position.y = wave.amplitude * sinR * edgeDampen;

    float waFactor = frequency * wave.amplitude;
    float radN = frequency * dot(float3(wave.direction), result.position) + time * phaseConstant;
    float sinN = sin(radN);
    float cosN = cos(radN);

    result.binormal.x = 1.0 - (qi * wave.direction.x * wave.direction.x * waFactor * sinN);
    result.binormal.z = -1.0 * (qi * wave.direction.x * wave.direction.z * waFactor * sinN);
    result.binormal.y = wave.direction.x * waFactor * cosN;

    result.tangent.x = -1.0 * (qi * wave.direction.x * wave.direction.z * waFactor * sinN);
    result.tangent.z = 1.0 - (qi * wave.direction.z * wave.direction.z * waFactor * sinN);
    result.tangent.y = wave.direction.z * waFactor * cosN;

    result.normal.x = -1.0 * (wave.direction.x * waFactor * cosN);
    result.normal.z = -1.0 * (wave.direction.z * waFactor * cosN);
    result.normal.y = 1.0 - (qi * waFactor * sinN);

    result.binormal = normalize(result.binormal);
    result.tangent = normalize(result.tangent);
    result.normal = normalize(result.normal);

    return result;
}

vertex WaterVSOut vertexMain(
    uint vertexID [[vertex_id]],
    constant CameraDataRHI& camera [[buffer(0)]],
    constant WaterDataRHI& water [[buffer(1)]],
    device const WaterVertexRHI* vertices [[buffer(2)]]
) {
    WaterVSOut out;

    float4 position = float4(float3(vertices[vertexID].position), 1.0);
    float4 texCoord0 = float4(float2(vertices[vertexID].uv0), float2(vertices[vertexID].uv1));

    float dampening = 1.0 - pow(saturate(abs(texCoord0.z - 0.5) / 0.5), water.dampeningFactor);
    dampening *= 1.0 - pow(saturate(abs(texCoord0.w - 0.5) / 0.5), water.dampeningFactor);

    WaveResultRHI finalWave;
    finalWave.position = float3(0.0);
    finalWave.normal = float3(0.0);
    finalWave.tangent = float3(0.0);
    finalWave.binormal = float3(0.0);

    uint numWaves = min(water.waveCount, 4u);
    for (uint waveId = 0u; waveId < numWaves; ++waveId) {
        WaveResultRHI w = calculateWaveRHI(water.waves[waveId], position.xyz, dampening, water.time, numWaves);
        finalWave.position += w.position;
        finalWave.normal += w.normal;
        finalWave.tangent += w.tangent;
        finalWave.binormal += w.binormal;
    }

    if (numWaves > 0u) {
        finalWave.position -= position.xyz * float(numWaves - 1u);
        finalWave.normal = normalize(finalWave.normal);
        finalWave.tangent = normalize(finalWave.tangent);
        finalWave.binormal = normalize(finalWave.binormal);
    } else {
        finalWave.position = position.xyz;
        finalWave.normal = float3(0.0, 1.0, 0.0);
        finalWave.tangent = float3(1.0, 0.0, 0.0);
        finalWave.binormal = float3(0.0, 0.0, 1.0);
    }

    out.worldNormalAndHeight.w = finalWave.position.y - position.y;

    position = float4(finalWave.position, 1.0);
    out.positionWorld = water.modelMatrix * position;
    out.positionView = camera.view * out.positionWorld;
    out.position = camera.proj * out.positionView;
    out.screenPosition = out.position;

    float3x3 normalMatrix = float3x3(
        water.modelMatrix[0].xyz,
        water.modelMatrix[1].xyz,
        water.modelMatrix[2].xyz
    );

    out.worldNormalAndHeight.xyz = normalize(normalMatrix * finalWave.normal);
    out.normalView = normalize((camera.view * float4(out.worldNormalAndHeight.xyz, 0.0)).xyz);
    out.tangentView = normalize((camera.view * float4(normalMatrix * finalWave.tangent, 0.0)).xyz);
    out.binormalView = normalize((camera.view * float4(normalMatrix * finalWave.binormal, 0.0)).xyz);

    out.texCoord0 = texCoord0;

    return out;
}

// ── Fragment helpers ────────────────────────────────────────────────────────

constant float WATER_PI = 3.14159265359;
constant float WATER_EPSILON = 0.0001;

static float waterNDGGX(float linearRoughness, float nDotH) {
    float a2 = linearRoughness * linearRoughness;
    float d = (nDotH * nDotH) * (a2 - 1.0) + 1.0;
    return a2 / (WATER_PI * d * d);
}

static float3 waterFresnel(float lDotH, float3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - lDotH, 5.0);
}

static float waterSmithGGX(float linearRoughness, float nDotL, float nDotV) {
    float k = linearRoughness * 0.5;
    float ggxL = nDotL / (nDotL * (1.0 - k) + k);
    float ggxV = nDotV / (nDotV * (1.0 - k) + k);
    return ggxL * ggxV;
}

static float2 waterClipToScreenUV(float4 clipPos) {
    float2 ndc = clipPos.xy / clipPos.w;
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

static float3 waterWorldPosFromDepth(float2 uv, float depth, constant CameraDataRHI& camera) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 viewPos = camera.invProj * float4(ndc, depth, 1.0);
    viewPos /= viewPos.w;
    return (camera.invView * float4(viewPos.xyz, 1.0)).xyz;
}

static float3 waterViewPosFromDepth(float2 uv, float depth, constant CameraDataRHI& camera) {
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 viewPos = camera.invProj * float4(ndc, depth, 1.0);
    return viewPos.xyz / viewPos.w;
}

fragment float4 fragmentMain(
    WaterVSOut in [[stage_in]],
    constant WaterDataRHI& water [[buffer(0)]],
    constant CameraDataRHI& camera [[buffer(1)]],
    texture2d<float, access::sample> waterNormalMap1 [[texture(0)]],
    texture2d<float, access::sample> waterNormalMap2 [[texture(1)]],
    texture2d<float, access::sample> sceneColor [[texture(2)]],
    texture2d<float, access::sample> sceneDepth [[texture(3)]],
    texture2d<float, access::sample> sceneNormal [[texture(4)]],
    texturecube<float, access::sample> envMap [[texture(5)]],
    texture2d<float, access::sample> foamMap [[texture(6)]],
    texture2d<float, access::sample> noiseMap [[texture(7)]]
) {
    constexpr sampler linearWrapSampler(address::repeat, filter::linear, mip_filter::linear);
    constexpr sampler pointClampSampler(address::clamp_to_edge, filter::nearest);
    constexpr sampler linearClampSampler(address::clamp_to_edge, filter::linear, mip_filter::linear);

    float3 normal = normalize(in.normalView);
    float3 tangent = normalize(in.tangentView);
    float3 binormal = normalize(in.binormalView);

    float2 normalMapCoords1 = in.texCoord0.xy + water.time * water.normalMapScroll.xy * water.normalMapScrollSpeed.x;
    float2 normalMapCoords2 = in.texCoord0.xy + water.time * water.normalMapScroll.zw * water.normalMapScrollSpeed.y;

    float2 hdrCoords = waterClipToScreenUV(in.screenPosition);

    float3 normalMap1 = waterNormalMap1.sample(linearWrapSampler, normalMapCoords1).rgb * 2.0 - 1.0;
    float3 normalMap2 = waterNormalMap2.sample(linearWrapSampler, normalMapCoords2).rgb * 2.0 - 1.0;
    float3x3 texSpace = float3x3(tangent, binormal, normal);
    float3 finalNormal = normalize(texSpace * normalMap1);
    finalNormal += normalize(texSpace * normalMap2);
    finalNormal = normalize(finalNormal);

    // ── Sun specular (GGX + sparkle noise) ──────────────────────────────────
    float linearRoughness = water.roughness * water.roughness;
    float3 viewDir = -normalize(in.positionView.xyz);
    float3 lightDir = -normalize((camera.view * float4(water.sunDirection.xyz, 0.0)).xyz);
    float3 halfVec = normalize(viewDir + lightDir);
    float nDotL = saturate(dot(finalNormal, lightDir));
    float nDotV = abs(dot(finalNormal, viewDir)) + WATER_EPSILON;
    float nDotH = saturate(dot(finalNormal, halfVec));
    float lDotH = saturate(dot(lightDir, halfVec));

    float3 f0 = float3(0.16 * water.reflectance * water.reflectance);
    float normalDistribution = waterNDGGX(linearRoughness, nDotH);
    float3 fresnelReflectance = waterFresnel(lDotH, f0);
    float geometryTerm = waterSmithGGX(linearRoughness, nDotL, nDotV);

    float specularNoise = noiseMap.sample(linearWrapSampler, normalMapCoords1 * 0.5).r;
    specularNoise *= noiseMap.sample(linearWrapSampler, normalMapCoords2 * 0.5).r;
    specularNoise *= noiseMap.sample(linearWrapSampler, in.texCoord0.xy * 0.5).r;

    float3 specularFactor = (geometryTerm * normalDistribution) * fresnelReflectance
                          * water.specIntensity * nDotL * specularNoise;

    // ── Screen-space reflections ────────────────────────────────────────────
    float3 reflectionVector = normalize(reflect(-viewDir, finalNormal));
    bool ssrEnabled = water.ssrSettings.y > 0.0;

    float3 rayMarchPosition = in.positionView.xyz;
    float2 hitUV = float2(0.0);
    float stepCount = 0.0;
    float forwardStepCount = water.ssrSettings.y;
    float3 finalSceneViewPos = float3(0.0);
    bool foundHit = false;

    if (ssrEnabled) {
        while (stepCount < water.ssrSettings.y) {
            rayMarchPosition += reflectionVector * water.ssrSettings.x;

            float4 rayClip = camera.proj * float4(rayMarchPosition, 1.0);
            if (abs(rayClip.w) < WATER_EPSILON) rayClip.w = WATER_EPSILON;

            if (abs(rayClip.x) > rayClip.w || abs(rayClip.y) > rayClip.w || rayClip.z > rayClip.w) {
                stepCount += 1.0;
                continue;
            }

            float2 rayUV = waterClipToScreenUV(rayClip);
            if (rayUV.x < 0.0 || rayUV.x > 1.0 || rayUV.y < 0.0 || rayUV.y > 1.0) {
                stepCount += 1.0;
                continue;
            }

            float sceneZ = sceneDepth.sample(pointClampSampler, rayUV).r;
            float3 sceneViewPos = waterViewPosFromDepth(rayUV, sceneZ, camera);

            if (sceneViewPos.z >= rayMarchPosition.z) {
                forwardStepCount = stepCount;
                finalSceneViewPos = sceneViewPos;
                hitUV = rayUV;
                foundHit = true;
                break;
            }
            stepCount += 1.0;
        }

        if (foundHit && forwardStepCount < water.ssrSettings.y) {
            float refineStep = 0.0;
            while (refineStep < water.ssrSettings.z) {
                rayMarchPosition -= reflectionVector * water.ssrSettings.x / water.ssrSettings.z;
                float4 rayClip = camera.proj * float4(rayMarchPosition, 1.0);
                if (abs(rayClip.w) < WATER_EPSILON) rayClip.w = WATER_EPSILON;
                float2 rayUV = clamp(waterClipToScreenUV(rayClip), float2(0.0), float2(1.0));

                float sceneZ = sceneDepth.sample(pointClampSampler, rayUV).r;
                float3 sceneViewPos = waterViewPosFromDepth(rayUV, sceneZ, camera);

                if (sceneViewPos.z < rayMarchPosition.z) {
                    break;
                }
                hitUV = rayUV;
                finalSceneViewPos = sceneViewPos;
                refineStep += 1.0;
            }
        }
    }

    float ssrFactor = 0.0;
    if (ssrEnabled && foundHit) {
        float3 ssrReflectionNormal = normalize(sceneNormal.sample(pointClampSampler, hitUV).xyz);
        float3 ssrReflectionNormalView = normalize((camera.view * float4(ssrReflectionNormal, 0.0)).xyz);
        float2 ssrDistanceFactor = float2(abs(0.5 - hdrCoords.x), abs(0.5 - hdrCoords.y)) * 2.0;

        float hitDistanceFactor = (forwardStepCount < water.ssrSettings.y)
            ? (1.0 - forwardStepCount / water.ssrSettings.y) : 0.0;
        float depthFactor = 1.0 / (1.0 + abs(finalSceneViewPos.z - rayMarchPosition.z) * water.ssrSettings.w);
        float normalFactor = 1.0 - saturate(dot(ssrReflectionNormalView, finalNormal));

        ssrFactor = (1.0 - abs(nDotV))
                  * hitDistanceFactor
                  * saturate(1.0 - ssrDistanceFactor.x - ssrDistanceFactor.y)
                  * depthFactor
                  * normalFactor;
    }

    float3 ssrColor = (foundHit && ssrFactor > 0.001)
        ? sceneColor.sample(pointClampSampler, hitUV).rgb : float3(0.0);

    // Environment fallback: the IBL prefiltered cubemap (split-sum LOD).
    float3 envReflectionDir = (camera.invView * float4(reflectionVector, 0.0)).xyz;
    float3 envColor = envMap.sample(linearClampSampler, envReflectionDir, level(water.roughness * 4.0)).rgb
                    * water.causticsBoundsMax.w;

    float3 reflectionColor = mix(envColor, ssrColor, saturate(ssrFactor)) * water.surfaceColor.rgb;

    // ── Refraction ──────────────────────────────────────────────────────────
    float2 distortedTexCoord = hdrCoords + ((finalNormal.xz + finalNormal.xy) * 0.5) * water.refractionDistortionFactor;
    float distortedDepth = sceneDepth.sample(pointClampSampler, distortedTexCoord).r;
    float3 distortedPosition = waterWorldPosFromDepth(distortedTexCoord, distortedDepth, camera);

    float2 refractionTexCoord = (distortedPosition.y < in.positionWorld.y) ? distortedTexCoord : hdrCoords;
    float3 waterColor = sceneColor.sample(linearClampSampler, refractionTexCoord).rgb * water.refractionColor.rgb;

    float sceneDepthSample = sceneDepth.sample(pointClampSampler, hdrCoords).r;
    float3 scenePosition = waterWorldPosFromDepth(hdrCoords, sceneDepthSample, camera);

    float depthSoftenedAlpha = saturate(length(scenePosition - in.positionWorld.xyz) / water.depthSofteningDistance);

    float3 waterSurfacePosition = (distortedPosition.y < in.positionWorld.y) ? distortedPosition : scenePosition;
    waterColor = mix(waterColor, water.refractionColor.rgb,
                     saturate((in.positionWorld.y - waterSurfacePosition.y) / water.refractionHeightFactor));

    // ── Combine ─────────────────────────────────────────────────────────────
    float3 geometricNormal = normalize(in.normalView);
    float waveTopReflectionFactor = pow(1.0 - saturate(dot(geometricNormal, viewDir)), 3.0);
    float reflectionBlend = saturate(saturate(length(in.positionView.xyz) / water.refractionDistanceFactor)
                                     + waveTopReflectionFactor);
    float3 waterBaseColor = mix(waterColor, reflectionColor, reflectionBlend);

    float3 finalWaterColor = waterBaseColor
                           + specularFactor * water.sunColorIntensity.rgb * water.sunColorIntensity.w;

    // ── Foam ────────────────────────────────────────────────────────────────
    float3 foamColor = foamMap.sample(linearWrapSampler, (normalMapCoords1 + normalMapCoords2) * water.foamTiling).rgb;
    float foamNoise = noiseMap.sample(linearWrapSampler, in.texCoord0.xy * water.foamTiling).r;

    float foamAmount = saturate((in.worldNormalAndHeight.w - water.foamHeightStart) / water.foamFadeDistance);
    foamAmount *= pow(saturate(dot(in.worldNormalAndHeight.xyz, float3(0.0, 1.0, 0.0))), water.foamAngleExponent);
    foamAmount *= foamNoise;
    foamAmount += pow(1.0 - depthSoftenedAlpha, 3.0);

    finalWaterColor = mix(finalWaterColor, foamColor * water.foamBrightness,
                          saturate(foamAmount) * depthSoftenedAlpha);

    return float4(finalWaterColor, depthSoftenedAlpha);
}
