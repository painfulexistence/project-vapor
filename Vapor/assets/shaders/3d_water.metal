#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"

// ============================================================================
// Water-specific data structures
// ============================================================================

struct WaterVertexData {
    packed_float3 position;
    packed_float2 uv0;  // Tiled UV for normal map scrolling
    packed_float2 uv1;  // Grid UV for edge dampening
};

struct WaveData {
    packed_float3 direction;
    float steepness;
    float waveLength;
    float amplitude;
    float speed;
    float _pad1;
};

struct WaterData {
    float4x4 modelMatrix;
    float4 surfaceColor;
    float4 refractionColor;
    float4 ssrSettings;
    float4 normalMapScroll;
    float2 normalMapScrollSpeed;
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
    float tessellationFactor;
    float dampeningFactor;
    float time;
    float _pad1;
    WaveData waves[4];
    uint waveCount;
    float _pad2[3];
};

struct WaterRasterizerData {
    float4 position [[position]];
    float3 normal;
    float3 tangent;
    float3 binormal;
    float4 positionView;
    float4 texCoord0;      // xy: tiled UV, zw: grid UV
    float4 screenPosition;
    float4 positionWorld;
    float4 worldNormalAndHeight;  // xyz: world normal, w: wave height
};

// ============================================================================
// Wave calculation (Gerstner waves based on GPU Gems)
// ============================================================================

struct WaveResult {
    float3 position;
    float3 normal;
    float3 binormal;
    float3 tangent;
};

WaveResult calculateWave(WaveData wave, float3 wavePosition, float edgeDampen, float time, uint numWaves) {
    WaveResult result;

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

// ============================================================================
// PBR helper functions for specular
// ============================================================================

float calculateNormalDistributionGGX(float linearRoughness, float nDotH) {
    float a2 = linearRoughness * linearRoughness;
    float d = (nDotH * nDotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float3 calculateSchlickFresnelReflectance(float lDotH, float3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - lDotH, 5.0);
}

float calculateSmithGGXGeometryTerm(float linearRoughness, float nDotL, float nDotV) {
    float k = linearRoughness * 0.5;
    float ggxL = nDotL / (nDotL * (1.0 - k) + k);
    float ggxV = nDotV / (nDotV * (1.0 - k) + k);
    return ggxL * ggxV;
}

// ============================================================================
// Utility functions
// ============================================================================

float3 getWorldPositionFromDepth(float2 uv, float depth, float4x4 invViewProj) {
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;
    float4 worldPos = invViewProj * clipPos;
    return worldPos.xyz / worldPos.w;
}

float3 getViewPositionFromDepth(float2 uv, float depth, float4x4 invProj) {
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;
    float4 viewPos = invProj * clipPos;
    return viewPos.xyz / viewPos.w;
}

// ============================================================================
// Vertex Shader
// ============================================================================

vertex WaterRasterizerData vertexMain(
    uint vertexID [[vertex_id]],
    constant CameraData& camera [[buffer(0)]],
    constant WaterData& water [[buffer(1)]],
    device const WaterVertexData* vertices [[buffer(2)]]
) {
    WaterRasterizerData out;

    float4 position = float4(vertices[vertexID].position, 1.0);
    float4 texCoord0 = float4(vertices[vertexID].uv0, vertices[vertexID].uv1);

    // Calculate edge dampening to clamp water motion at edges
    float dampening = 1.0 - pow(saturate(abs(texCoord0.z - 0.5) / 0.5), water.dampeningFactor);
    dampening *= 1.0 - pow(saturate(abs(texCoord0.w - 0.5) / 0.5), water.dampeningFactor);

    // Accumulate wave results
    WaveResult finalWaveResult;
    finalWaveResult.position = float3(0, 0, 0);
    finalWaveResult.normal = float3(0, 0, 0);
    finalWaveResult.tangent = float3(0, 0, 0);
    finalWaveResult.binormal = float3(0, 0, 0);

    uint numWaves = min(water.waveCount, 4u);
    for (uint waveId = 0; waveId < numWaves; ++waveId) {
        WaveResult waveResult = calculateWave(water.waves[waveId], position.xyz, dampening, water.time, numWaves);
        finalWaveResult.position += waveResult.position;
        finalWaveResult.normal += waveResult.normal;
        finalWaveResult.tangent += waveResult.tangent;
        finalWaveResult.binormal += waveResult.binormal;
    }

    // Normalize accumulated results (subtract base position for each wave beyond the first)
    if (numWaves > 0) {
        finalWaveResult.position -= position.xyz * float(numWaves - 1);
        finalWaveResult.normal = normalize(finalWaveResult.normal);
        finalWaveResult.tangent = normalize(finalWaveResult.tangent);
        finalWaveResult.binormal = normalize(finalWaveResult.binormal);
    } else {
        finalWaveResult.position = position.xyz;
        finalWaveResult.normal = float3(0, 1, 0);
        finalWaveResult.tangent = float3(1, 0, 0);
        finalWaveResult.binormal = float3(0, 0, 1);
    }

    // Store wave height for later use (foam calculation)
    out.worldNormalAndHeight.w = finalWaveResult.position.y - position.y;

    // Transform to world/view/clip space
    position = float4(finalWaveResult.position, 1.0);
    out.positionWorld = water.modelMatrix * position;
    out.positionView = camera.view * out.positionWorld;
    out.position = camera.proj * out.positionView;
    out.screenPosition = out.position;

    // Transform normals/tangents
    float3x3 normalMatrix = float3x3(
        water.modelMatrix[0].xyz,
        water.modelMatrix[1].xyz,
        water.modelMatrix[2].xyz
    );

    out.worldNormalAndHeight.xyz = normalize(normalMatrix * finalWaveResult.normal);
    out.normal = normalize((camera.view * float4(out.worldNormalAndHeight.xyz, 0.0)).xyz);
    out.tangent = normalize((camera.view * float4(normalMatrix * finalWaveResult.tangent, 0.0)).xyz);
    out.binormal = normalize((camera.view * float4(normalMatrix * finalWaveResult.binormal, 0.0)).xyz);

    out.texCoord0 = texCoord0;

    return out;
}

// ============================================================================
// Fragment Shader
// ============================================================================

fragment float4 fragmentMain(
    WaterRasterizerData in [[stage_in]],
    texture2d<float, access::sample> waterNormalMap1 [[texture(0)]],
    texture2d<float, access::sample> waterNormalMap2 [[texture(1)]],
    texture2d<float, access::sample> hdrMap [[texture(2)]],
    texture2d<float, access::sample> depthMap [[texture(3)]],
    texture2d<float, access::sample> normalMap [[texture(4)]],
    texturecube<float, access::sample> environmentMap [[texture(5)]],
    texture2d<float, access::sample> foamMap [[texture(6)]],
    texture2d<float, access::sample> noiseMap [[texture(7)]],
    constant WaterData& water [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]],
    constant DirLight* directionalLights [[buffer(2)]],
    constant float2& screenSize [[buffer(3)]]
) {
    constexpr sampler linearWrapSampler(address::repeat, filter::linear, mip_filter::linear);
    constexpr sampler pointClampSampler(address::clamp_to_edge, filter::nearest);
    constexpr sampler linearClampSampler(address::clamp_to_edge, filter::linear);

    const float EPSILON = 0.0001;

    // Normalize interpolated vectors
    float3 normal = normalize(in.normal);
    float3 tangent = normalize(in.tangent);
    float3 binormal = normalize(in.binormal);

    // Calculate scrolling normal map coordinates
    float2 normalMapCoords1 = in.texCoord0.xy + water.time * water.normalMapScroll.xy * water.normalMapScrollSpeed.x;
    float2 normalMapCoords2 = in.texCoord0.xy + water.time * water.normalMapScroll.zw * water.normalMapScrollSpeed.y;

    // Screen space coordinates for sampling HDR and depth
    float2 hdrCoords = ((float2(in.screenPosition.x, -in.screenPosition.y) / in.screenPosition.w) * 0.5) + 0.5;

    // Sample and blend normal maps
    float3 normalMap1 = (waterNormalMap1.sample(linearWrapSampler, normalMapCoords1).rgb * 2.0) - 1.0;
    float3 normalMap2 = (waterNormalMap2.sample(linearWrapSampler, normalMapCoords2).rgb * 2.0) - 1.0;

    float3x3 texSpace = float3x3(tangent, binormal, normal);
    float3 finalNormal = normalize(texSpace * normalMap1.xyz);
    finalNormal += normalize(texSpace * normalMap2.xyz);
    finalNormal = normalize(finalNormal);

    // ========================================================================
    // Specular lighting with noise for sparkle effect
    // ========================================================================

    float linearRoughness = water.roughness * water.roughness;
    float3 viewDir = -normalize(in.positionView.xyz);
    float3 lightDir = -normalize((camera.view * float4(directionalLights[0].direction, 0.0)).xyz);
    float3 halfVec = normalize(viewDir + lightDir);
    float nDotL = saturate(dot(finalNormal, lightDir));
    float nDotV = abs(dot(finalNormal, viewDir)) + EPSILON;
    float nDotH = saturate(dot(finalNormal, halfVec));
    float lDotH = saturate(dot(lightDir, halfVec));

    float3 f0 = float3(0.16 * water.reflectance * water.reflectance);
    float normalDistribution = calculateNormalDistributionGGX(linearRoughness, nDotH);
    float3 fresnelReflectance = calculateSchlickFresnelReflectance(lDotH, f0);
    float geometryTerm = calculateSmithGGXGeometryTerm(linearRoughness, nDotL, nDotV);

    // Sample noise for sparkle effect
    float specularNoise = noiseMap.sample(linearWrapSampler, normalMapCoords1 * 0.5).r;
    specularNoise *= noiseMap.sample(linearWrapSampler, normalMapCoords2 * 0.5).r;
    specularNoise *= noiseMap.sample(linearWrapSampler, in.texCoord0.xy * 0.5).r;

    float3 specularFactor = (geometryTerm * normalDistribution) * fresnelReflectance * water.specIntensity * nDotL * specularNoise;

    // ========================================================================
    // Reflections (environment map + simplified SSR)
    // ========================================================================

    float3 reflectionVector = normalize(reflect(-viewDir, finalNormal));

    // Simple SSR ray march (simplified for performance)
    float3 rayMarchPosition = in.positionView.xyz;
    float4 rayMarchTexPosition = float4(0, 0, 0, 0);
    float stepCount = 0;
    float forwardStepCount = water.ssrSettings.y;
    float sceneZ = 0;

    while (stepCount < water.ssrSettings.y) {
        rayMarchPosition += reflectionVector.xyz * water.ssrSettings.x;
        rayMarchTexPosition = camera.proj * float4(-rayMarchPosition, 1);

        if (abs(rayMarchTexPosition.w) < EPSILON) {
            rayMarchTexPosition.w = EPSILON;
        }

        rayMarchTexPosition.xy /= rayMarchTexPosition.w;
        rayMarchTexPosition.xy = float2(rayMarchTexPosition.x, -rayMarchTexPosition.y) * 0.5 + 0.5;

        sceneZ = depthMap.sample(pointClampSampler, rayMarchTexPosition.xy).r;
        float3 sceneViewPos = getViewPositionFromDepth(rayMarchTexPosition.xy, sceneZ, camera.invProj);

        if (sceneViewPos.z <= rayMarchPosition.z) {
            forwardStepCount = stepCount;
            stepCount = water.ssrSettings.y;
        } else {
            stepCount++;
        }
    }

    // Refinement step
    if (forwardStepCount < water.ssrSettings.y) {
        stepCount = 0;
        while (stepCount < water.ssrSettings.z) {
            rayMarchPosition -= reflectionVector.xyz * water.ssrSettings.x / water.ssrSettings.z;
            rayMarchTexPosition = camera.proj * float4(-rayMarchPosition, 1);

            if (abs(rayMarchTexPosition.w) < EPSILON) {
                rayMarchTexPosition.w = EPSILON;
            }

            rayMarchTexPosition.xy /= rayMarchTexPosition.w;
            rayMarchTexPosition.xy = float2(rayMarchTexPosition.x, -rayMarchTexPosition.y) * 0.5 + 0.5;

            sceneZ = depthMap.sample(pointClampSampler, rayMarchTexPosition.xy).r;
            float3 sceneViewPos = getViewPositionFromDepth(rayMarchTexPosition.xy, sceneZ, camera.invProj);

            if (sceneViewPos.z > rayMarchPosition.z) {
                stepCount = water.ssrSettings.z;
            } else {
                stepCount++;
            }
        }
    }

    // Calculate SSR blend factor
    float3 ssrReflectionNormal = normalMap.sample(pointClampSampler, rayMarchTexPosition.xy).xyz * 2.0 - 1.0;
    float2 ssrDistanceFactor = float2(distance(0.5, hdrCoords.x), distance(0.5, hdrCoords.y)) * 2.0;
    float ssrFactor = (1.0 - abs(nDotV))
                      * (1.0 - forwardStepCount / water.ssrSettings.y)
                      * saturate(1.0 - ssrDistanceFactor.x - ssrDistanceFactor.y)
                      * (1.0 / (1.0 + abs(sceneZ - rayMarchPosition.z) * water.ssrSettings.w))
                      * (1.0 - saturate(dot(ssrReflectionNormal, finalNormal)));

    // Blend SSR with environment reflection
    float3 reflectionColor = hdrMap.sample(linearClampSampler, rayMarchTexPosition.xy).rgb;
    float3 envReflectionDir = (camera.invView * float4(reflectionVector, 0.0)).xyz;
    float3 skyboxColor = environmentMap.sample(linearClampSampler, envReflectionDir).rgb;
    reflectionColor = mix(skyboxColor, reflectionColor, saturate(ssrFactor)) * water.surfaceColor.rgb;

    // ========================================================================
    // Refraction
    // ========================================================================

    float4x4 viewProjInv = camera.invView * camera.invProj;

    // Distort refraction based on normal
    float2 distortedTexCoord = hdrCoords + ((finalNormal.xz + finalNormal.xy) * 0.5) * water.refractionDistortionFactor;
    float distortedDepth = depthMap.sample(pointClampSampler, distortedTexCoord).r;
    float3 distortedPosition = getWorldPositionFromDepth(distortedTexCoord, distortedDepth, viewProjInv);

    // Check if distorted position is above water surface (invalid)
    float2 refractionTexCoord = (distortedPosition.y < in.positionWorld.y) ? distortedTexCoord : hdrCoords;
    float3 waterColor = hdrMap.sample(linearClampSampler, refractionTexCoord).rgb * water.refractionColor.rgb;

    // Get scene depth for depth-based effects
    float sceneDepth = depthMap.sample(pointClampSampler, hdrCoords).r;
    float3 scenePosition = getWorldPositionFromDepth(hdrCoords, sceneDepth, viewProjInv);

    // Depth softening for alpha
    float depthSoftenedAlpha = saturate(distance(scenePosition, in.positionWorld.xyz) / water.depthSofteningDistance);

    // Fade refraction based on water depth
    float3 waterSurfacePosition = (distortedPosition.y < in.positionWorld.y) ? distortedPosition : scenePosition;
    waterColor = mix(waterColor, water.refractionColor.rgb, saturate((in.positionWorld.y - waterSurfacePosition.y) / water.refractionHeightFactor));

    // ========================================================================
    // Blend reflections and refractions
    // ========================================================================

    float waveTopReflectionFactor = pow(1.0 - saturate(dot(normalize(in.normal), viewDir)), 3.0);
    float3 waterBaseColor = mix(waterColor, reflectionColor, saturate(saturate(length(in.positionView.xyz) / water.refractionDistanceFactor) + waveTopReflectionFactor));

    // Add specular
    float3 finalWaterColor = waterBaseColor + specularFactor * directionalLights[0].color * directionalLights[0].intensity;

    // ========================================================================
    // Foam
    // ========================================================================

    float3 foamColor = foamMap.sample(linearWrapSampler, (normalMapCoords1 + normalMapCoords2) * water.foamTiling).rgb;
    float foamNoise = noiseMap.sample(linearWrapSampler, in.texCoord0.xy * water.foamTiling).r;

    // Height-based foam
    float foamAmount = saturate((in.worldNormalAndHeight.w - water.foamHeightStart) / water.foamFadeDistance);
    // Angle-based foam (only on top of waves)
    foamAmount *= pow(saturate(dot(in.worldNormalAndHeight.xyz, float3(0, 1, 0))), water.foamAngleExponent);
    // Apply noise for bubbly look
    foamAmount *= foamNoise;

    // Shore foam (where water meets geometry)
    foamAmount += pow((1.0 - depthSoftenedAlpha), 3.0);

    // Blend foam with water
    finalWaterColor = mix(finalWaterColor, foamColor * water.foamBrightness, saturate(foamAmount) * depthSoftenedAlpha);

    return float4(finalWaterColor, depthSoftenedAlpha);
}
