#include <metal_stdlib>
using namespace metal;
#include "assets/shaders/3d_common.metal"
#include "assets/shaders/3d_volumetric_common.metal"

// ============================================================================
// Volumetric Fog - Froxel-based Implementation
// ============================================================================
// Uses a 3D froxel (frustum voxel) grid to compute scattering and extinction.
// The grid is aligned to the camera frustum with exponential depth distribution.

// Froxel grid dimensions
constant uint FROXEL_SIZE_X = 160;
constant uint FROXEL_SIZE_Y = 90;
constant uint FROXEL_SIZE_Z = 64;

// ============================================================================
// Data Structures
// ============================================================================

struct VolumetricFogData {
    float4x4 invViewProj;           // Inverse view-projection matrix
    float4x4 prevViewProj;          // Previous frame view-projection (for TAA)
    float3 cameraPosition;          // Camera world position
    float3 sunDirection;            // Sun direction (normalized)
    float3 sunColor;                // Sun color
    float sunIntensity;             // Sun light intensity

    // Fog parameters
    float fogDensity;               // Base fog density
    float fogHeightFalloff;         // Height-based density falloff
    float fogBaseHeight;            // Height where fog is densest
    float fogMaxHeight;             // Maximum height for fog

    // Scattering parameters
    float scatteringCoeff;          // Scattering coefficient
    float extinctionCoeff;          // Extinction coefficient (absorption + out-scatter)
    float anisotropy;               // Phase function anisotropy (g parameter)
    float ambientIntensity;         // Ambient light contribution

    // Grid parameters
    float nearPlane;                // Camera near plane
    float farPlane;                 // Camera far plane (fog far)
    float2 screenSize;              // Screen dimensions

    // Temporal
    uint frameIndex;                // Frame counter for temporal jitter
    float temporalBlend;            // Blend factor for TAA (0.05-0.1)

    // Noise parameters
    float noiseScale;               // Scale of density noise
    float noiseIntensity;           // Intensity of density variation
    float windSpeed;                // Wind animation speed
    float time;                     // Current time
    float3 windDirection;           // Wind direction
};

struct FroxelData {
    float4 scatteringExtinction;    // RGB: in-scattered light, A: transmittance
};

// ============================================================================
// Helper Functions
// ============================================================================

// Convert froxel index to world position
float3 froxelToWorld(uint3 froxelIdx, constant VolumetricFogData& data) {
    // Normalized froxel coordinates [0, 1]
    float3 uvw = (float3(froxelIdx) + 0.5) / float3(FROXEL_SIZE_X, FROXEL_SIZE_Y, FROXEL_SIZE_Z);

    // Exponential depth distribution (more slices near camera)
    float linearDepth = data.nearPlane * pow(data.farPlane / data.nearPlane, uvw.z);

    // Screen space to clip space
    float2 ndc = uvw.xy * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Flip Y for Metal

    // Reconstruct world position
    float4 clipPos = float4(ndc * linearDepth, linearDepth, 1.0);
    float4 viewPos = data.invViewProj * clipPos;
    return viewPos.xyz / viewPos.w;
}

// Get depth slice from linear depth
float depthToSlice(float linearDepth, float nearPlane, float farPlane) {
    return log(linearDepth / nearPlane) / log(farPlane / nearPlane) * FROXEL_SIZE_Z;
}

// Sample fog density at world position
float sampleFogDensity(float3 worldPos, constant VolumetricFogData& data) {
    float height = worldPos.y;

    // Base height fog density
    float heightDensity = data.fogDensity * heightFogDensity(height, data.fogBaseHeight, data.fogHeightFalloff);

    // Clamp to max height
    if (height > data.fogMaxHeight) {
        return 0.0;
    }

    // Add noise for variation
    float3 noisePos = worldPos * data.noiseScale + data.windDirection * data.time * data.windSpeed;
    float noise = fbm3D(noisePos, 4, 2.0, 0.5);
    noise = noise * 0.5 + 0.5;  // Remap to [0, 1]

    // Combine density
    float density = heightDensity * (1.0 + (noise - 0.5) * data.noiseIntensity * 2.0);

    return max(0.0, density);
}

// ============================================================================
// Compute Shader: Froxel Injection
// ============================================================================
// Computes density and lighting for each froxel

kernel void froxelInjection(
    uint3 gid [[thread_position_in_grid]],
    constant VolumetricFogData& data [[buffer(0)]],
    constant DirLight* dirLights [[buffer(1)]],
    constant PointLight* pointLights [[buffer(2)]],
    constant uint& dirLightCount [[buffer(3)]],
    constant uint& pointLightCount [[buffer(4)]],
    device FroxelData* froxelGrid [[buffer(5)]],
    texture2d<float, access::sample> shadowMap [[texture(0)]]
) {
    // Bounds check
    if (gid.x >= FROXEL_SIZE_X || gid.y >= FROXEL_SIZE_Y || gid.z >= FROXEL_SIZE_Z) {
        return;
    }

    // Froxel linear index
    uint froxelIdx = gid.z * FROXEL_SIZE_X * FROXEL_SIZE_Y + gid.y * FROXEL_SIZE_X + gid.x;

    // Get world position of froxel center
    float3 worldPos = froxelToWorld(gid, data);

    // Sample density
    float density = sampleFogDensity(worldPos, data);

    // Early out for zero density
    if (density < 0.0001) {
        froxelGrid[froxelIdx].scatteringExtinction = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Calculate scattering and extinction coefficients
    float scattering = density * data.scatteringCoeff;
    float extinction = density * data.extinctionCoeff;

    // View direction (from froxel to camera)
    float3 viewDir = normalize(data.cameraPosition - worldPos);

    // Accumulate light contribution
    float3 inScatteredLight = float3(0.0);

    // Sun light contribution
    {
        float cosTheta = dot(viewDir, data.sunDirection);
        float phase = phaseHenyeyGreenstein(cosTheta, data.anisotropy);

        // TODO: Add shadow sampling for volumetric shadows
        float shadow = 1.0;

        inScatteredLight += data.sunColor * data.sunIntensity * phase * shadow;
    }

    // Point light contributions (simplified - could use shadow maps)
    for (uint i = 0; i < min(pointLightCount, 32u); i++) {
        float3 lightDir = pointLights[i].position - worldPos;
        float lightDist = length(lightDir);
        lightDir /= lightDist;

        // Attenuation
        float attenuation = 1.0 / (1.0 + lightDist * lightDist / (pointLights[i].radius * pointLights[i].radius));

        // Phase function
        float cosTheta = dot(viewDir, lightDir);
        float phase = phaseHenyeyGreenstein(cosTheta, data.anisotropy);

        inScatteredLight += pointLights[i].color * pointLights[i].intensity * phase * attenuation;
    }

    // Add ambient light
    float3 ambient = mix(float3(0.1, 0.1, 0.15), data.sunColor * 0.1, saturate(worldPos.y * 0.01));
    inScatteredLight += ambient * data.ambientIntensity;

    // Scale by scattering coefficient
    inScatteredLight *= scattering;

    // Store result
    froxelGrid[froxelIdx].scatteringExtinction = float4(inScatteredLight, extinction);
}

// ============================================================================
// Compute Shader: Scattering Integration
// ============================================================================
// Ray marches through froxel grid and integrates scattering

kernel void scatteringIntegration(
    uint3 gid [[thread_position_in_grid]],
    constant VolumetricFogData& data [[buffer(0)]],
    device FroxelData* froxelGrid [[buffer(1)]],
    texture3d<float, access::write> integratedVolume [[texture(0)]]
) {
    // Bounds check
    if (gid.x >= FROXEL_SIZE_X || gid.y >= FROXEL_SIZE_Y) {
        return;
    }

    // March from near to far, accumulating scattering
    float3 accumulatedScattering = float3(0.0);
    float accumulatedTransmittance = 1.0;

    // Calculate step size (exponential distribution)
    for (uint z = 0; z < FROXEL_SIZE_Z; z++) {
        uint froxelIdx = z * FROXEL_SIZE_X * FROXEL_SIZE_Y + gid.y * FROXEL_SIZE_X + gid.x;

        float4 scatterExtinct = froxelGrid[froxelIdx].scatteringExtinction;
        float3 inScatter = scatterExtinct.rgb;
        float extinction = scatterExtinct.a;

        // Calculate slice depth for step size
        float sliceDepthNear = data.nearPlane * pow(data.farPlane / data.nearPlane, float(z) / FROXEL_SIZE_Z);
        float sliceDepthFar = data.nearPlane * pow(data.farPlane / data.nearPlane, float(z + 1) / FROXEL_SIZE_Z);
        float stepSize = sliceDepthFar - sliceDepthNear;

        // Transmittance through this slice
        float sliceTransmittance = exp(-extinction * stepSize);

        // Integrate scattering (analytical integration)
        // Using the formula: integral of S * exp(-extinction * t) dt
        float3 sliceScattering = inScatter * (1.0 - sliceTransmittance) / max(extinction, 0.0001);

        // Accumulate
        accumulatedScattering += accumulatedTransmittance * sliceScattering;
        accumulatedTransmittance *= sliceTransmittance;

        // Write integrated result for this slice
        integratedVolume.write(float4(accumulatedScattering, accumulatedTransmittance), uint3(gid.xy, z));
    }
}

// ============================================================================
// Fragment Shader: Apply Fog to Scene
// ============================================================================

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// Full screen triangle vertices
constant float2 fsTriVerts[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0)
};

vertex VertexOut volumetricFogVertex(uint vertexID [[vertex_id]]) {
    VertexOut out;
    out.position = float4(fsTriVerts[vertexID], 0.0, 1.0);
    out.uv = fsTriVerts[vertexID] * 0.5 + 0.5;
    out.uv.y = 1.0 - out.uv.y;
    return out;
}

fragment float4 volumetricFogFragment(
    VertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],
    texture2d<float, access::sample> sceneDepth [[texture(1)]],
    texture3d<float, access::sample> integratedVolume [[texture(2)]],
    constant VolumetricFogData& data [[buffer(0)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    // Sample scene
    float4 color = sceneColor.sample(linearSampler, in.uv);
    float depth = sceneDepth.sample(linearSampler, in.uv).r;

    // Linearize depth
    float linearDepth = data.nearPlane * data.farPlane / (data.farPlane - depth * (data.farPlane - data.nearPlane));

    // Calculate volume UV
    float3 volumeUV;
    volumeUV.xy = in.uv;
    volumeUV.z = saturate(depthToSlice(linearDepth, data.nearPlane, data.farPlane) / FROXEL_SIZE_Z);

    // Sample integrated volume
    float4 fogData = integratedVolume.sample(linearSampler, volumeUV);
    float3 inScattering = fogData.rgb;
    float transmittance = fogData.a;

    // Apply fog: final = scene * transmittance + inScattering
    float3 result = color.rgb * transmittance + inScattering;

    return float4(result, color.a);
}

// ============================================================================
// Simplified Single-Pass Fog (Alternative for lower-end hardware)
// ============================================================================

fragment float4 simpleFogFragment(
    VertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneColor [[texture(0)]],
    texture2d<float, access::sample> sceneDepth [[texture(1)]],
    constant VolumetricFogData& data [[buffer(0)]],
    constant CameraData& camera [[buffer(1)]]
) {
    constexpr sampler linearSampler(filter::linear, address::clamp_to_edge);

    float4 color = sceneColor.sample(linearSampler, in.uv);
    float depth = sceneDepth.sample(linearSampler, in.uv).r;

    // Skip sky pixels (no fog at infinity)
    if (depth >= 0.9999) {
        return color;
    }

    // Reconstruct world position using invViewProj
    float2 ndc = in.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Metal Y-flip
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos4 = data.invViewProj * clipPos;
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    // Calculate fog
    float3 rayDir = normalize(worldPos - data.cameraPosition);
    float distance = length(worldPos - data.cameraPosition);
    float height = worldPos.y;

    // Height-based exponential fog
    float fogAmount = exponentialHeightFog(distance, height, data.fogDensity, data.fogHeightFalloff);

    // Clamp fog amount to reasonable range
    fogAmount = saturate(fogAmount);

    // Phase function for sun (forward scattering effect)
    float cosTheta = dot(rayDir, data.sunDirection);
    float phase = phaseHenyeyGreenstein(cosTheta, data.anisotropy);

    // Fog color (sun contribution + ambient)
    float3 fogColor = data.sunColor * data.sunIntensity * phase * 0.1 + float3(0.5, 0.6, 0.7) * data.ambientIntensity;

    // Apply fog
    float3 result = mix(color.rgb, fogColor, fogAmount);

    return float4(result, color.a);
}
