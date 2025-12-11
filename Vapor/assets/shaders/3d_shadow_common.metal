#ifndef SHADOW_COMMON_METAL
#define SHADOW_COMMON_METAL

#include <metal_stdlib>
using namespace metal;

// ============================================================================
// Shadow Map Constants
// ============================================================================

constant uint CSM_CASCADE_COUNT = 4;
constant uint CSM_SHADOW_MAP_SIZE = 2048;
constant float CSM_BIAS_SCALE = 0.005;

// ============================================================================
// Shadow Data Structures (must match C++ side)
// ============================================================================

struct CascadeShadowData {
    float4x4 lightViewProj[CSM_CASCADE_COUNT];
    float4 cascadeSplits;
    float4 cascadeScales[CSM_CASCADE_COUNT];
    float3 lightDirection;
    float shadowBias;
    float normalBias;
    float pcfRadius;
    uint cascadeCount;
    uint pcfSampleCount;
    float cascadeBlendRange;
    float maxShadowDistance;
    uint softShadowEnabled;
    uint cascadeVisualization;
};

struct HybridShadowData {
    float penumbraThresholdLow;
    float penumbraThresholdHigh;
    float rtSampleRadius;
    uint rtSampleCount;
    float contactShadowLength;
    float contactShadowThickness;
    uint hybridEnabled;
    float _pad1;
};

// ============================================================================
// Poisson Disk Samples for PCF
// ============================================================================

constant float2 poissonDisk4[4] = {
    float2(-0.94201624, -0.39906216),
    float2(0.94558609, -0.76890725),
    float2(-0.094184101, -0.92938870),
    float2(0.34495938, 0.29387760)
};

constant float2 poissonDisk8[8] = {
    float2(-0.326212, -0.40581),
    float2(-0.840144, -0.07358),
    float2(-0.695914, 0.457137),
    float2(-0.203345, 0.620716),
    float2(0.96234, -0.194983),
    float2(0.473434, -0.480026),
    float2(0.519456, 0.767022),
    float2(0.185461, -0.893124)
};

constant float2 poissonDisk16[16] = {
    float2(-0.94201624, -0.39906216),
    float2(0.94558609, -0.76890725),
    float2(-0.094184101, -0.92938870),
    float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432),
    float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845),
    float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554),
    float2(0.53742981, -0.47373420),
    float2(-0.26496911, -0.41893023),
    float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507),
    float2(-0.81409955, 0.91437590),
    float2(0.19984126, 0.78641367),
    float2(0.14383161, -0.14100790)
};

constant float2 poissonDisk32[32] = {
    float2(-0.975402, -0.0711386),
    float2(-0.920347, -0.41142),
    float2(-0.883908, 0.217872),
    float2(-0.884518, 0.568041),
    float2(-0.811945, 0.90521),
    float2(-0.792474, -0.779962),
    float2(-0.614856, 0.386578),
    float2(-0.580859, -0.208777),
    float2(-0.53795, 0.716666),
    float2(-0.515427, 0.0899991),
    float2(-0.454634, -0.707938),
    float2(-0.420942, 0.991272),
    float2(-0.261147, 0.588488),
    float2(-0.211219, 0.114841),
    float2(-0.146336, -0.259194),
    float2(-0.139439, -0.888668),
    float2(0.0116886, 0.326395),
    float2(0.0380566, 0.625477),
    float2(0.0625477, -0.50853),
    float2(0.125509, 0.0469069),
    float2(0.169797, -0.332196),
    float2(0.229255, 0.631921),
    float2(0.231957, -0.00459782),
    float2(0.264392, -0.795663),
    float2(0.316463, 0.923407),
    float2(0.363779, 0.292169),
    float2(0.404239, -0.519219),
    float2(0.530161, 0.726564),
    float2(0.557603, -0.183661),
    float2(0.639212, 0.358948),
    float2(0.713539, -0.52011),
    float2(0.869889, -0.227173)
};

// ============================================================================
// Utility Functions
// ============================================================================

// Generate a random rotation angle for rotated PCF
float randomAngle(float2 screenPos, uint frameIndex) {
    float2 noise = fract(sin(dot(screenPos + float(frameIndex) * 0.1, float2(12.9898, 78.233))) * 43758.5453);
    return noise.x * 2.0 * M_PI_F;
}

// Rotate a 2D vector by an angle
float2 rotateVector(float2 v, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return float2(v.x * c - v.y * s, v.x * s + v.y * c);
}

// ============================================================================
// Cascade Selection
// ============================================================================

// Select the appropriate cascade based on view-space depth
uint selectCascade(float viewDepth, float4 cascadeSplits, uint cascadeCount) {
    for (uint i = 0; i < cascadeCount; i++) {
        if (viewDepth < cascadeSplits[i]) {
            return i;
        }
    }
    return cascadeCount - 1;
}

// Calculate blend factor between cascades for smooth transitions
float calculateCascadeBlend(float viewDepth, float4 cascadeSplits, uint cascade, float blendRange) {
    if (cascade >= CSM_CASCADE_COUNT - 1) return 0.0;

    float splitDistance = cascadeSplits[cascade];
    float blendStart = splitDistance * (1.0 - blendRange);

    if (viewDepth > blendStart) {
        return smoothstep(blendStart, splitDistance, viewDepth);
    }
    return 0.0;
}

// ============================================================================
// Shadow Map Sampling
// ============================================================================

// Basic shadow map lookup (hard shadow)
float sampleShadowMapHard(
    depth2d_array<float, access::sample> shadowMap,
    sampler shadowSampler,
    float3 shadowCoord,
    uint cascade,
    float bias
) {
    if (shadowCoord.x < 0.0 || shadowCoord.x > 1.0 ||
        shadowCoord.y < 0.0 || shadowCoord.y > 1.0 ||
        shadowCoord.z < 0.0 || shadowCoord.z > 1.0) {
        return 1.0; // Outside shadow map = lit
    }

    float shadowDepth = shadowMap.sample(shadowSampler, shadowCoord.xy, cascade);
    return (shadowCoord.z - bias > shadowDepth) ? 0.0 : 1.0;
}

// PCF (Percentage-Closer Filtering) with Poisson disk sampling
float sampleShadowMapPCF(
    depth2d_array<float, access::sample> shadowMap,
    sampler shadowSampler,
    float3 shadowCoord,
    uint cascade,
    float bias,
    float radius,
    uint sampleCount,
    float2 screenPos
) {
    if (shadowCoord.x < 0.0 || shadowCoord.x > 1.0 ||
        shadowCoord.y < 0.0 || shadowCoord.y > 1.0 ||
        shadowCoord.z < 0.0 || shadowCoord.z > 1.0) {
        return 1.0;
    }

    float texelSize = 1.0 / float(CSM_SHADOW_MAP_SIZE);
    float filterRadius = radius * texelSize;

    // Random rotation for temporal stability
    float angle = randomAngle(screenPos, 0);

    float shadow = 0.0;

    if (sampleCount <= 4) {
        for (uint i = 0; i < 4; i++) {
            float2 offset = rotateVector(poissonDisk4[i], angle) * filterRadius;
            float2 samplePos = shadowCoord.xy + offset;
            float shadowDepth = shadowMap.sample(shadowSampler, samplePos, cascade);
            shadow += (shadowCoord.z - bias > shadowDepth) ? 0.0 : 1.0;
        }
        shadow /= 4.0;
    } else if (sampleCount <= 8) {
        for (uint i = 0; i < 8; i++) {
            float2 offset = rotateVector(poissonDisk8[i], angle) * filterRadius;
            float2 samplePos = shadowCoord.xy + offset;
            float shadowDepth = shadowMap.sample(shadowSampler, samplePos, cascade);
            shadow += (shadowCoord.z - bias > shadowDepth) ? 0.0 : 1.0;
        }
        shadow /= 8.0;
    } else if (sampleCount <= 16) {
        for (uint i = 0; i < 16; i++) {
            float2 offset = rotateVector(poissonDisk16[i], angle) * filterRadius;
            float2 samplePos = shadowCoord.xy + offset;
            float shadowDepth = shadowMap.sample(shadowSampler, samplePos, cascade);
            shadow += (shadowCoord.z - bias > shadowDepth) ? 0.0 : 1.0;
        }
        shadow /= 16.0;
    } else {
        for (uint i = 0; i < 32; i++) {
            float2 offset = rotateVector(poissonDisk32[i], angle) * filterRadius;
            float2 samplePos = shadowCoord.xy + offset;
            float shadowDepth = shadowMap.sample(shadowSampler, samplePos, cascade);
            shadow += (shadowCoord.z - bias > shadowDepth) ? 0.0 : 1.0;
        }
        shadow /= 32.0;
    }

    return shadow;
}

// ============================================================================
// CSM Shadow Calculation (Main Entry Point)
// ============================================================================

float calculateCSMShadow(
    float3 worldPos,
    float3 worldNormal,
    float viewDepth,
    float2 screenPos,
    constant CascadeShadowData& csmData,
    depth2d_array<float, access::sample> shadowMap,
    sampler shadowSampler
) {
    // Select cascade
    uint cascade = selectCascade(viewDepth, csmData.cascadeSplits, csmData.cascadeCount);

    // Transform to light space
    float4 lightSpacePos = csmData.lightViewProj[cascade] * float4(worldPos, 1.0);
    float3 shadowCoord = lightSpacePos.xyz / lightSpacePos.w;

    // Convert from NDC [-1,1] to texture space [0,1]
    shadowCoord.xy = shadowCoord.xy * 0.5 + 0.5;
    shadowCoord.y = 1.0 - shadowCoord.y; // Flip Y for Metal texture coordinates

    // Calculate bias based on surface angle to light
    float NdotL = max(dot(worldNormal, -csmData.lightDirection), 0.0);
    float slopeScale = clamp(1.0 - NdotL, 0.0, 1.0);
    float bias = csmData.shadowBias + csmData.normalBias * slopeScale;

    // Increase bias for farther cascades to reduce artifacts
    bias *= float(cascade + 1);

    // Sample shadow
    float shadow;
    if (csmData.softShadowEnabled) {
        shadow = sampleShadowMapPCF(
            shadowMap, shadowSampler, shadowCoord, cascade,
            bias, csmData.pcfRadius, csmData.pcfSampleCount, screenPos
        );
    } else {
        shadow = sampleShadowMapHard(shadowMap, shadowSampler, shadowCoord, cascade, bias);
    }

    // Blend between cascades for smooth transitions
    float blendFactor = calculateCascadeBlend(viewDepth, csmData.cascadeSplits, cascade, csmData.cascadeBlendRange);
    if (blendFactor > 0.0 && cascade < csmData.cascadeCount - 1) {
        // Sample next cascade
        uint nextCascade = cascade + 1;
        float4 nextLightSpacePos = csmData.lightViewProj[nextCascade] * float4(worldPos, 1.0);
        float3 nextShadowCoord = nextLightSpacePos.xyz / nextLightSpacePos.w;
        nextShadowCoord.xy = nextShadowCoord.xy * 0.5 + 0.5;
        nextShadowCoord.y = 1.0 - nextShadowCoord.y;

        float nextBias = bias * float(nextCascade + 1) / float(cascade + 1);

        float nextShadow;
        if (csmData.softShadowEnabled) {
            nextShadow = sampleShadowMapPCF(
                shadowMap, shadowSampler, nextShadowCoord, nextCascade,
                nextBias, csmData.pcfRadius, csmData.pcfSampleCount, screenPos
            );
        } else {
            nextShadow = sampleShadowMapHard(shadowMap, shadowSampler, nextShadowCoord, nextCascade, nextBias);
        }

        shadow = mix(shadow, nextShadow, blendFactor);
    }

    // Fade out shadow at max distance
    float distanceFade = 1.0 - smoothstep(csmData.maxShadowDistance * 0.8, csmData.maxShadowDistance, viewDepth);
    shadow = mix(1.0, shadow, distanceFade);

    return shadow;
}

// ============================================================================
// Debug Visualization
// ============================================================================

float3 getCascadeDebugColor(uint cascade) {
    constant float3 cascadeColors[4] = {
        float3(1.0, 0.0, 0.0), // Red - cascade 0 (nearest)
        float3(0.0, 1.0, 0.0), // Green - cascade 1
        float3(0.0, 0.0, 1.0), // Blue - cascade 2
        float3(1.0, 1.0, 0.0)  // Yellow - cascade 3 (farthest)
    };
    return cascadeColors[min(cascade, 3u)];
}

#endif // SHADOW_COMMON_METAL
