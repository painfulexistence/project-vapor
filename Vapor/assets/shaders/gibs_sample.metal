#include <metal_stdlib>
#include "gibs_common.metal"
#include "3d_common.metal"
using namespace metal;

// ============================================================================
// GIBS Screen-Space Sampling Shader
// Samples indirect lighting from surfels for each screen pixel
//
// Design Decision: Direct query with half-resolution optimization
// - Each pixel queries nearby surfels from spatial hash
// - Half resolution for performance, bilateral upsample for quality
// - Weight by distance and normal similarity
// See GIBS_DESIGN.md Decision #5
// ============================================================================

// Sample GI from surfels for a single pixel
float3 sampleGIFromSurfels(
    float3 worldPos,
    float3 normal,
    device const Surfel* surfels,
    device const SurfelCell* cells,
    constant GIBSData& gibs,
    constant GIBSSampleParams& params
) {
    // Check world bounds
    if (!isInWorldBounds(worldPos, gibs)) {
        return float3(0);
    }

    // Get cell for this position
    uint3 baseCell = worldToCell(worldPos, gibs.worldMin, gibs.cellSize);

    float3 irradianceSum = float3(0);
    float weightSum = 0.0;

    // Search in neighboring cells
    int searchRadius = int(params.sampleRadius);

    for (int dz = -searchRadius; dz <= searchRadius; dz++) {
        for (int dy = -searchRadius; dy <= searchRadius; dy++) {
            for (int dx = -searchRadius; dx <= searchRadius; dx++) {
                int3 cellCoord = int3(baseCell) + int3(dx, dy, dz);

                // Bounds check
                if (any(cellCoord < 0) || any(uint3(cellCoord) >= gibs.gridSize)) {
                    continue;
                }

                uint cellIndex = cellToIndex(uint3(cellCoord), gibs.gridSize);
                if (cellIndex >= gibs.totalCells) {
                    continue;
                }

                SurfelCell cell = cells[cellIndex];

                if (cell.surfelCount == 0) {
                    continue;
                }

                // Sample surfels in this cell
                uint maxSamples = min(cell.surfelCount, params.maxSamples);

                for (uint i = 0; i < maxSamples; i++) {
                    uint surfelIndex = cell.surfelOffset + i;
                    Surfel surfel = surfels[surfelIndex];

                    if (!(surfel.flags & SURFEL_FLAG_VALID)) {
                        continue;
                    }

                    // Compute weight
                    float weight = computeSurfelWeight(worldPos, normal, surfel, params.sampleRadius * gibs.cellSize);

                    if (weight > GIBS_EPSILON) {
                        // Accumulate irradiance (direct + indirect)
                        float3 surfelRadiance = surfel.irradiance + surfel.directLight;
                        irradianceSum += surfelRadiance * weight;
                        weightSum += weight;
                    }
                }
            }
        }
    }

    // Normalize
    if (weightSum > GIBS_EPSILON) {
        return irradianceSum / weightSum;
    }

    return float3(0);
}

// Main GI sampling kernel (writes to GI buffer at reduced resolution)
kernel void giSample(
    texture2d<float, access::read> depthTexture [[texture(0)]],
    texture2d<float, access::read> normalTexture [[texture(1)]],
    texture2d<float, access::write> giOutput [[texture(2)]],
    device const Surfel* surfels [[buffer(0)]],
    device const SurfelCell* cells [[buffer(1)]],
    constant GIBSData& gibs [[buffer(2)]],
    constant GIBSSampleParams& params [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]]
) {
    if (gid.x >= uint(params.giResolution.x) || gid.y >= uint(params.giResolution.y)) {
        return;
    }

    // Calculate corresponding screen UV
    float2 uv = (float2(gid) + 0.5) / params.giResolution;

    // Sample depth at full resolution
    uint2 depthCoord = uint2(uv * params.screenSize);
    float depth = depthTexture.read(depthCoord).r;

    // Skip sky pixels
    if (depth > 0.9999) {
        giOutput.write(float4(0, 0, 0, 0), gid);
        return;
    }

    // Reconstruct world position
    float3 worldPos = reconstructWorldPosition(uv, depth, params.invViewProj);

    // Read normal
    float3 normal = normalTexture.read(depthCoord).xyz * 2.0 - 1.0;
    normal = normalize(normal);

    // Sample GI from surfels
    float3 gi = sampleGIFromSurfels(worldPos, normal, surfels, cells, gibs, params);

    giOutput.write(float4(gi, 1.0), gid);
}

// Bilateral upsample from half-res GI to full-res
// Preserves edges using depth and normal comparison
kernel void giBilateralUpsample(
    texture2d<float, access::read> giLowRes [[texture(0)]],
    texture2d<float, access::read> depthTexture [[texture(1)]],
    texture2d<float, access::read> normalTexture [[texture(2)]],
    texture2d<float, access::write> giFullRes [[texture(3)]],
    constant GIBSSampleParams& params [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]]
) {
    if (gid.x >= uint(params.screenSize.x) || gid.y >= uint(params.screenSize.y)) {
        return;
    }

    // Read center pixel properties
    float centerDepth = depthTexture.read(gid).r;
    float3 centerNormal = normalTexture.read(gid).xyz * 2.0 - 1.0;

    // Skip sky
    if (centerDepth > 0.9999) {
        giFullRes.write(float4(0, 0, 0, 0), gid);
        return;
    }

    // Calculate low-res UV
    float2 uv = (float2(gid) + 0.5) / params.screenSize;
    float2 lowResUV = uv * params.giResolution;
    int2 lowResBase = int2(floor(lowResUV - 0.5));

    float3 giSum = float3(0);
    float weightSum = 0.0;

    // 2x2 bilinear with bilateral weights
    for (int dy = 0; dy <= 1; dy++) {
        for (int dx = 0; dx <= 1; dx++) {
            int2 lowResCoord = lowResBase + int2(dx, dy);

            // Bounds check
            if (lowResCoord.x < 0 || lowResCoord.x >= int(params.giResolution.x) ||
                lowResCoord.y < 0 || lowResCoord.y >= int(params.giResolution.y)) {
                continue;
            }

            // Bilinear weight
            float2 bilinearWeight = 1.0 - abs(lowResUV - float2(lowResCoord) - 0.5);
            float spatialWeight = bilinearWeight.x * bilinearWeight.y;

            // Get corresponding full-res position for depth/normal comparison
            uint2 sampleFullRes = uint2((float2(lowResCoord) + 0.5) / params.giResolution * params.screenSize);
            sampleFullRes = clamp(sampleFullRes, uint2(0), uint2(params.screenSize) - 1);

            float sampleDepth = depthTexture.read(sampleFullRes).r;
            float3 sampleNormal = normalTexture.read(sampleFullRes).xyz * 2.0 - 1.0;

            // Depth weight (penalize large depth differences)
            float depthDiff = abs(centerDepth - sampleDepth);
            float depthWeight = exp(-depthDiff * 1000.0); // Steep falloff

            // Normal weight
            float normalDot = max(0.0, dot(centerNormal, sampleNormal));
            float normalWeight = pow(normalDot, 4.0);

            // Combined weight
            float weight = spatialWeight * depthWeight * normalWeight;

            if (weight > GIBS_EPSILON) {
                float3 gi = giLowRes.read(uint2(lowResCoord)).rgb;
                giSum += gi * weight;
                weightSum += weight;
            }
        }
    }

    float3 result = float3(0);
    if (weightSum > GIBS_EPSILON) {
        result = giSum / weightSum;
    }

    giFullRes.write(float4(result, 1.0), gid);
}

// Apply GI to scene (composite with direct lighting)
kernel void giComposite(
    texture2d<float, access::read> sceneColor [[texture(0)]],
    texture2d<float, access::read> giTexture [[texture(1)]],
    texture2d<float, access::read> albedoTexture [[texture(2)]],
    texture2d<float, access::write> outputColor [[texture(3)]],
    constant GIBSData& gibs [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]]
) {
    uint2 size = uint2(gibs.screenSize);
    if (gid.x >= size.x || gid.y >= size.y) {
        return;
    }

    float4 scene = sceneColor.read(gid);
    float3 gi = giTexture.read(gid).rgb;
    float3 albedo = albedoTexture.read(gid).rgb;

    // Add indirect diffuse contribution
    // GI is already multiplied by surfel albedo, so just add it
    float3 result = scene.rgb + gi;

    outputColor.write(float4(result, scene.a), gid);
}

// Debug visualization: show surfels as colored points
kernel void debugVisualizeSurfels(
    texture2d<float, access::read> sceneColor [[texture(0)]],
    texture2d<float, access::read> depthTexture [[texture(1)]],
    texture2d<float, access::write> outputColor [[texture(2)]],
    device const Surfel* surfels [[buffer(0)]],
    constant GIBSData& gibs [[buffer(1)]],
    constant CameraData& camera [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]]
) {
    uint2 size = uint2(gibs.screenSize);
    if (gid.x >= size.x || gid.y >= size.y) {
        return;
    }

    float4 scene = sceneColor.read(gid);
    float sceneDepth = depthTexture.read(gid).r;

    // Check each surfel to see if it projects to this pixel
    float3 result = scene.rgb;

    for (uint i = 0; i < min(gibs.activeSurfelCount, 10000u); i++) {
        Surfel surfel = surfels[i];

        if (!(surfel.flags & SURFEL_FLAG_VALID)) {
            continue;
        }

        // Project surfel to screen
        float4 clip = camera.proj * camera.view * float4(surfel.position, 1.0);
        float3 ndc = clip.xyz / clip.w;

        // Check if visible
        if (ndc.z < 0.0 || ndc.z > 1.0) {
            continue;
        }

        float2 screenPos = (ndc.xy * 0.5 + 0.5) * gibs.screenSize;
        screenPos.y = gibs.screenSize.y - screenPos.y;

        // Check if this pixel is near the surfel
        float dist = length(float2(gid) - screenPos);
        float screenRadius = 3.0; // Fixed screen-space radius for debug

        if (dist < screenRadius) {
            // Draw surfel color (irradiance)
            float alpha = 1.0 - dist / screenRadius;
            float3 surfelColor = surfel.irradiance + surfel.directLight;
            surfelColor = max(surfelColor, debugSurfelColor(surfel.flags) * 0.5);
            result = mix(result, surfelColor, alpha * 0.8);
        }
    }

    outputColor.write(float4(result, scene.a), gid);
}
