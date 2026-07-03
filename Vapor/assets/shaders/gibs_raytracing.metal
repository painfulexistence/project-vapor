#include <metal_stdlib>
#include <metal_raytracing>
#include "Res/shaders/gibs_common.metal"
#include "Res/shaders/3d_common.metal"
using namespace metal;

// ============================================================================
// GIBS Raytracing Shader
// Computes indirect lighting by tracing rays between surfels
//
// Design Decision: 4 rays per surfel with temporal accumulation
// - Lower ray count per frame for performance
// - Temporal accumulation over 8+ frames for convergence
// - Cosine-weighted hemisphere sampling for importance sampling
// See GIBS_DESIGN.md Decision #4
// ============================================================================

// Compute direct lighting for a surfel
float3 computeDirectLight(float3 position, float3 normal, float3 albedo,
                           float3 sunDir, float3 sunColor, float sunIntensity,
                           raytracing::instance_acceleration_structure accelStruct) {
    // Shadow ray to sun
    raytracing::ray shadowRay;
    shadowRay.origin = position + normal * 0.001; // Bias to avoid self-intersection
    shadowRay.direction = sunDir;
    shadowRay.min_distance = 0.001;
    shadowRay.max_distance = 10000.0;

    raytracing::intersector<raytracing::instancing> shadowIntersector;
    shadowIntersector.assume_geometry_type(raytracing::geometry_type::triangle);
    shadowIntersector.accept_any_intersection(true); // Early out on any hit

    auto shadowResult = shadowIntersector.intersect(shadowRay, accelStruct, 0xFF);

    float shadow = (shadowResult.type == raytracing::intersection_type::none) ? 1.0 : 0.0;

    // Simple Lambertian direct lighting
    float NdotL = max(0.0, dot(normal, sunDir));
    return albedo * sunColor * sunIntensity * NdotL * shadow * GIBS_INV_PI;
}

// Find nearest surfel at hit position using spatial hash
// Returns irradiance from that surfel
float3 sampleSurfelAtPosition(float3 hitPos, float3 hitNormal,
                                device const Surfel* surfels,
                                device const SurfelCell* cells,
                                constant GIBSData& gibs) {
    // Get cell for hit position
    if (!isInWorldBounds(hitPos, gibs)) {
        return float3(0);
    }

    uint cellHash = computeCellHash(hitPos, gibs);
    if (cellHash >= gibs.totalCells) {
        return float3(0);
    }

    SurfelCell cell = cells[cellHash];

    if (cell.surfelCount == 0) {
        return float3(0);
    }

    // Find best matching surfel in this cell
    float bestWeight = 0.0;
    float3 bestIrradiance = float3(0);

    uint maxCheck = min(cell.surfelCount, 16u); // Limit iterations

    for (uint i = 0; i < maxCheck; i++) {
        uint surfelIndex = cell.surfelOffset + i;
        Surfel surfel = surfels[surfelIndex];

        if (!(surfel.flags & SURFEL_FLAG_VALID)) {
            continue;
        }

        // Compute weight based on distance and normal similarity
        float weight = computeSurfelWeight(hitPos, hitNormal, surfel, gibs.cellSize * 2.0);

        if (weight > bestWeight) {
            bestWeight = weight;
            // Return surfel's outgoing radiance (irradiance * albedo for diffuse)
            bestIrradiance = surfel.irradiance + surfel.directLight;
        }
    }

    return bestIrradiance * bestWeight;
}

// Main surfel raytracing kernel
// surfels: canonical buffer (read/write) — irradiance accumulates here across frames.
// sortedSurfels + cells: last frame's spatial hash for neighbor irradiance lookup
// (cell offsets index into the SORTED buffer, so it must be bound separately).
kernel void surfelRaytracing(
    device Surfel* surfels [[buffer(0)]],
    device const SurfelCell* cells [[buffer(1)]],
    constant GIBSData& gibs [[buffer(2)]],
    constant SurfelRaytracingParams& params [[buffer(3)]],
    raytracing::instance_acceleration_structure accelStruct [[buffer(4)]],
    device const Surfel* sortedSurfels [[buffer(5)]],
    uint gid [[thread_position_in_grid]]
) {
    uint surfelIndex = params.surfelOffset + gid;

    if (surfelIndex >= gibs.activeSurfelCount) {
        return;
    }

    Surfel surfel = surfels[surfelIndex];

    if (!(surfel.flags & SURFEL_FLAG_VALID)) {
        return;
    }

    // Compute direct lighting first
    surfel.directLight = computeDirectLight(
        surfel.position, surfel.normal, surfel.albedo,
        gibs.sunDirection, gibs.sunColor, gibs.sunIntensity,
        accelStruct
    );

    // Accumulate indirect lighting from multiple rays
    float3 indirectSum = float3(0);
    uint validRays = 0;

    for (uint rayIndex = 0; rayIndex < params.raysPerSurfel; rayIndex++) {
        // Generate random direction (cosine-weighted hemisphere)
        uint seed = surfelIndex * 1000 + rayIndex + params.frameIndex * 100000;
        float2 u = randomFloat2(seed);
        float3 rayDir = sampleCosineHemisphere(u, surfel.normal);

        // Create ray
        raytracing::ray r;
        r.origin = surfel.position + surfel.normal * params.rayBias;
        r.direction = rayDir;
        r.min_distance = 0.001;
        r.max_distance = params.rayMaxDistance;

        // Trace ray
        raytracing::intersector<raytracing::instancing, raytracing::triangle_data> inter;
        inter.assume_geometry_type(raytracing::geometry_type::triangle);
        auto result = inter.intersect(r, accelStruct, 0xFF);

        if (result.type == raytracing::intersection_type::triangle) {
            // Hit geometry - sample surfel at hit point
            float3 hitPos = r.origin + r.direction * result.distance;

            // Approximate hit normal (would need geometry data for exact)
            // For now, use a simple heuristic or assume facing camera
            float3 hitNormal = -r.direction;

            // Get irradiance from surfel at hit position (last frame's sorted hash)
            float3 hitRadiance = sampleSurfelAtPosition(hitPos, hitNormal, sortedSurfels, cells, gibs);

            // Cosine term already included in importance sampling
            indirectSum += hitRadiance * surfel.albedo;
            validRays++;
        } else {
            // Hit sky - sample environment (simplified)
            // In production, sample from environment cubemap
            float3 skyColor = float3(0.5, 0.7, 1.0) * 0.3; // Simple sky color
            indirectSum += skyColor * surfel.albedo;
            validRays++;
        }
    }

    // Average indirect contribution
    float3 newIndirect = float3(0);
    if (validRays > 0) {
        newIndirect = indirectSum / float(validRays);
    }

    // Temporal blending with previous irradiance
    // Design Decision: Temporal accumulation for noise reduction
    surfel.irradiance = mix(surfel.irradiance, newIndirect, gibs.temporalBlend);
    surfel.age += 1.0;

    // Write back to the canonical buffer so accumulation persists across frames
    surfels[surfelIndex] = surfel;
}

// Simplified version without acceleration structure (for debugging)
kernel void surfelRaytracingSimple(
    device Surfel* surfels [[buffer(0)]],
    device const SurfelCell* cells [[buffer(1)]],
    constant GIBSData& gibs [[buffer(2)]],
    constant SurfelRaytracingParams& params [[buffer(3)]],
    uint gid [[thread_position_in_grid]]
) {
    uint surfelIndex = params.surfelOffset + gid;

    if (surfelIndex >= gibs.activeSurfelCount) {
        return;
    }

    Surfel surfel = surfels[surfelIndex];

    if (!(surfel.flags & SURFEL_FLAG_VALID)) {
        return;
    }

    // Simple direct lighting without shadows
    float NdotL = max(0.0, dot(surfel.normal, gibs.sunDirection));
    surfel.directLight = surfel.albedo * gibs.sunColor * gibs.sunIntensity * NdotL * GIBS_INV_PI;

    // Simple ambient indirect (placeholder until RT works)
    float3 ambient = float3(0.1, 0.12, 0.15);
    surfel.irradiance = mix(surfel.irradiance, ambient * surfel.albedo, gibs.temporalBlend);
    surfel.age += 1.0;

    surfels[surfelIndex] = surfel;
}
