#include <metal_stdlib>
#include "gibs_common.metal"
#include "3d_common.metal"
using namespace metal;

// ============================================================================
// GIBS Surfel Generation Shader
// Generates surfels from G-buffer (depth + normal + albedo)
//
// Design Decision: Hybrid generation strategy
// - Static surfels generated once and maintained across frames
// - Dynamic surfels regenerated each frame for moving objects
// See GIBS_DESIGN.md Decision #3
// ============================================================================

// Counter indices
constant uint COUNTER_NEW_SURFELS = 0;
constant uint COUNTER_TOTAL_SURFELS = 1;

// Stochastic surfel generation based on density
// Uses blue noise pattern for better distribution
kernel void surfelGeneration(
    texture2d<float, access::read> depthTexture [[texture(0)]],
    texture2d<float, access::read> normalTexture [[texture(1)]],
    texture2d<float, access::read> albedoTexture [[texture(2)]],
    device Surfel* surfels [[buffer(0)]],
    device atomic_uint* counters [[buffer(1)]],
    constant GIBSData& gibs [[buffer(2)]],
    constant SurfelGenerationParams& params [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]],
    uint2 gridSize [[threads_per_grid]]
) {
    // Bounds check
    if (gid.x >= uint(params.screenSize.x) || gid.y >= uint(params.screenSize.y)) {
        return;
    }

    // Read depth
    float depth = depthTexture.read(gid).r;

    // Skip sky pixels (depth = 1.0 or very close)
    if (depth > 0.9999) {
        return;
    }

    // Reconstruct world position
    float3 worldPos = reconstructWorldPositionFromPixel(gid, depth, params.screenSize, params.invViewProj);

    // Check world bounds
    if (!isInWorldBounds(worldPos, gibs)) {
        return;
    }

    // Read normal (stored as [0,1], convert to [-1,1])
    float3 normal = normalTexture.read(gid).xyz * 2.0 - 1.0;
    normal = normalize(normal);

    // Skip invalid normals
    if (length(normal) < 0.5) {
        return;
    }

    // Read albedo
    float4 albedoSample = albedoTexture.read(gid);
    float3 albedo = albedoSample.rgb;

    // Stochastic acceptance based on density
    // Design Decision: Probabilistic generation for uniform distribution
    uint seed = temporalSeed(gid, params.frameIndex);
    float rand = randomFloat(seed);

    // Density modulation based on:
    // 1. Base density parameter
    // 2. Distance to camera (fewer surfels far away)
    // 3. Surface angle to camera (fewer on grazing angles)
    float3 toCamera = normalize(gibs.cameraPosition - worldPos);
    float distToCamera = length(gibs.cameraPosition - worldPos);

    // Distance-based density falloff
    float distanceFactor = saturate(1.0 - distToCamera / gibs.rayMaxDistance);
    distanceFactor = distanceFactor * distanceFactor; // Quadratic falloff

    // Angle-based density (prefer surfaces facing camera)
    float angleFactor = max(0.1, dot(normal, toCamera));

    // Combined probability
    float probability = params.densityThreshold * distanceFactor * angleFactor;

    // Stochastic rejection
    if (rand > probability) {
        return;
    }

    // Allocate surfel index atomically
    uint surfelIndex = atomic_fetch_add_explicit(&counters[COUNTER_NEW_SURFELS], 1, memory_order_relaxed);

    // Check capacity
    if (surfelIndex >= params.maxNewSurfels || surfelIndex >= gibs.maxSurfels) {
        return;
    }

    // Calculate cell hash
    uint cellHash = computeCellHash(worldPos, gibs);

    // Create new surfel
    Surfel surfel;
    surfel.position = worldPos;
    surfel.radius = params.surfelRadius;
    surfel.normal = normal;
    surfel._pad1 = 0;
    surfel.albedo = albedo;
    surfel._pad2 = 0;
    surfel.irradiance = float3(0); // Will be filled by raytracing pass
    surfel._pad3 = 0;
    surfel.directLight = float3(0); // Will be filled by raytracing pass
    surfel.age = 0;
    surfel.cellHash = cellHash;
    surfel.flags = SURFEL_FLAG_STATIC | SURFEL_FLAG_VALID | SURFEL_FLAG_NEEDS_UPDATE;
    surfel.instanceID = 0; // Could be set from instance ID texture if available
    surfel._pad4 = 0;

    // Write surfel
    surfels[surfelIndex] = surfel;
}

// Update existing surfels (mark for update, age, etc.)
kernel void surfelUpdate(
    device Surfel* surfels [[buffer(0)]],
    device atomic_uint* counters [[buffer(1)]],
    constant GIBSData& gibs [[buffer(2)]],
    uint gid [[thread_position_in_grid]]
) {
    if (gid >= gibs.activeSurfelCount) {
        return;
    }

    Surfel surfel = surfels[gid];

    // Skip invalid surfels
    if (!(surfel.flags & SURFEL_FLAG_VALID)) {
        return;
    }

    // Increment age
    surfel.age += 1.0;

    // Mark static surfels that haven't been updated recently for raytracing
    // Design Decision: Stagger updates across frames for performance
    if (surfel.flags & SURFEL_FLAG_STATIC) {
        // Update every N frames based on age
        uint updateInterval = 4; // Update 1/4 of surfels per frame
        if (uint(surfel.age) % updateInterval == gibs.frameIndex % updateInterval) {
            surfel.flags |= SURFEL_FLAG_NEEDS_UPDATE;
        }
    }

    // Dynamic surfels always need update
    if (surfel.flags & SURFEL_FLAG_DYNAMIC) {
        surfel.flags |= SURFEL_FLAG_NEEDS_UPDATE;
    }

    // Cull surfels that are too old or out of bounds
    if (surfel.age > 1000.0 || !isInWorldBounds(surfel.position, gibs)) {
        surfel.flags &= ~SURFEL_FLAG_VALID;
    }

    surfels[gid] = surfel;
}

// Compact valid surfels (remove invalid ones)
// This is a parallel stream compaction - simplified version
kernel void surfelCompact(
    device Surfel* surfelsIn [[buffer(0)]],
    device Surfel* surfelsOut [[buffer(1)]],
    device atomic_uint* counters [[buffer(2)]],
    constant GIBSData& gibs [[buffer(3)]],
    uint gid [[thread_position_in_grid]]
) {
    if (gid >= gibs.activeSurfelCount) {
        return;
    }

    Surfel surfel = surfelsIn[gid];

    // Only copy valid surfels
    if (surfel.flags & SURFEL_FLAG_VALID) {
        uint outIndex = atomic_fetch_add_explicit(&counters[COUNTER_TOTAL_SURFELS], 1, memory_order_relaxed);
        if (outIndex < gibs.maxSurfels) {
            surfelsOut[outIndex] = surfel;
        }
    }
}
