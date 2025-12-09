#ifndef GIBS_COMMON_METAL
#define GIBS_COMMON_METAL

#include <metal_stdlib>
using namespace metal;

// ============================================================================
// GIBS (Global Illumination Based on Surfels) - Common Definitions
// Design documentation: docs/GIBS_DESIGN.md
// ============================================================================

// Surfel flags (matches SurfelFlags enum in graphics.hpp)
constant uint SURFEL_FLAG_NONE = 0;
constant uint SURFEL_FLAG_STATIC = 1 << 0;
constant uint SURFEL_FLAG_DYNAMIC = 1 << 1;
constant uint SURFEL_FLAG_NEEDS_UPDATE = 1 << 2;
constant uint SURFEL_FLAG_VALID = 1 << 3;

// Constants
constant float GIBS_PI = 3.14159265359;
constant float GIBS_INV_PI = 0.31830988618;
constant float GIBS_EPSILON = 1e-6;

// ============================================================================
// Data Structures (must match C++ side exactly)
// ============================================================================

// Single Surfel - 128 bytes
// Design Decision: 128 bytes for balance between info and memory efficiency
struct Surfel {
    float3 position;        // World space position
    float radius;           // Surfel coverage radius
    float3 normal;          // Surface normal (normalized)
    float _pad1;
    float3 albedo;          // Surface reflectance (diffuse color)
    float _pad2;
    float3 irradiance;      // Accumulated indirect irradiance (RGB)
    float _pad3;
    float3 directLight;     // Direct lighting contribution
    float age;              // Frame age for temporal stability
    uint cellHash;          // Spatial hash cell ID
    uint flags;             // SurfelFlags bitmask
    uint instanceID;        // Source mesh instance
    uint _pad4;
};

// Spatial hash cell for fast surfel lookup
struct SurfelCell {
    uint surfelOffset;      // Start index in sorted surfel array
    uint surfelCount;       // Number of surfels in this cell
    uint _pad[2];
};

// GIBS global parameters
struct GIBSData {
    float4x4 invViewProj;
    float4x4 prevViewProj;
    float3 cameraPosition;
    float _pad1;

    float3 sunDirection;
    float _pad2;
    float3 sunColor;
    float sunIntensity;

    uint maxSurfels;
    uint activeSurfelCount;
    float surfelRadius;
    float surfelDensity;

    float3 worldMin;
    float cellSize;
    float3 worldMax;
    uint totalCells;
    uint3 gridSize;
    float _pad3;

    uint raysPerSurfel;
    uint maxBounces;
    float rayBias;
    float rayMaxDistance;

    float temporalBlend;
    float hysteresis;
    uint frameIndex;
    float _pad4;

    float2 screenSize;
    float2 giResolution;

    uint sampleRadius;
    uint maxSurfelsPerPixel;
    float2 _pad5;
};

// Surfel generation parameters
struct SurfelGenerationParams {
    float4x4 invViewProj;
    float2 screenSize;
    float surfelRadius;
    float densityThreshold;
    uint maxNewSurfels;
    uint frameIndex;
    float2 _pad;
};

// Surfel raytracing parameters
struct SurfelRaytracingParams {
    uint surfelOffset;
    uint surfelCount;
    uint raysPerSurfel;
    uint frameIndex;
    float rayBias;
    float rayMaxDistance;
    float2 _pad;
};

// Screen-space sampling parameters
struct GIBSSampleParams {
    float4x4 invViewProj;
    float2 screenSize;
    float2 giResolution;
    float sampleRadius;
    uint maxSamples;
    float normalWeight;
    float distanceWeight;
};

// ============================================================================
// Spatial Hash Functions
// Design Decision: Uniform spatial hash chosen over octree for GPU efficiency
// ============================================================================

// Compute 3D grid cell coordinates from world position
inline uint3 worldToCell(float3 worldPos, float3 worldMin, float cellSize) {
    float3 relPos = worldPos - worldMin;
    return uint3(max(float3(0), floor(relPos / cellSize)));
}

// Compute linear cell index from 3D coordinates
inline uint cellToIndex(uint3 cell, uint3 gridSize) {
    return cell.x + cell.y * gridSize.x + cell.z * gridSize.x * gridSize.y;
}

// Compute cell hash from world position
inline uint computeCellHash(float3 worldPos, constant GIBSData& gibs) {
    uint3 cell = worldToCell(worldPos, gibs.worldMin, gibs.cellSize);
    // Clamp to grid bounds
    cell = min(cell, gibs.gridSize - 1);
    return cellToIndex(cell, gibs.gridSize);
}

// Check if position is within world bounds
inline bool isInWorldBounds(float3 pos, constant GIBSData& gibs) {
    return all(pos >= gibs.worldMin) && all(pos <= gibs.worldMax);
}

// ============================================================================
// Random Number Generation
// ============================================================================

// PCG hash for random number generation
inline uint pcgHash(uint input) {
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Generate random float in [0, 1]
inline float randomFloat(uint seed) {
    return float(pcgHash(seed)) / 4294967295.0;
}

// Generate random float2 in [0, 1]^2
inline float2 randomFloat2(uint seed) {
    uint h1 = pcgHash(seed);
    uint h2 = pcgHash(seed + 1);
    return float2(float(h1), float(h2)) / 4294967295.0;
}

// Generate random float3 in [0, 1]^3
inline float3 randomFloat3(uint seed) {
    uint h1 = pcgHash(seed);
    uint h2 = pcgHash(seed + 1);
    uint h3 = pcgHash(seed + 2);
    return float3(float(h1), float(h2), float(h3)) / 4294967295.0;
}

// Temporal-stable random seed
inline uint temporalSeed(uint2 pixel, uint frameIndex) {
    return pixel.x + pixel.y * 65536 + frameIndex * 16777216;
}

// ============================================================================
// Hemisphere Sampling
// ============================================================================

// Uniform hemisphere sampling
inline float3 sampleUniformHemisphere(float2 u, float3 normal) {
    float z = u.x;
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = 2.0 * GIBS_PI * u.y;

    float3 dir = float3(r * cos(phi), r * sin(phi), z);

    // Build TBN from normal
    float3 up = abs(normal.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);

    return tangent * dir.x + bitangent * dir.y + normal * dir.z;
}

// Cosine-weighted hemisphere sampling (for diffuse)
// Design Decision: Used for surfel raytracing to importance sample diffuse BRDF
inline float3 sampleCosineHemisphere(float2 u, float3 normal) {
    float r = sqrt(u.x);
    float theta = 2.0 * GIBS_PI * u.y;

    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(max(0.0, 1.0 - u.x));

    // Build TBN from normal
    float3 up = abs(normal.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);

    return normalize(tangent * x + bitangent * y + normal * z);
}

// PDF for cosine-weighted sampling
inline float cosineHemispherePDF(float cosTheta) {
    return cosTheta * GIBS_INV_PI;
}

// ============================================================================
// World Position Reconstruction
// ============================================================================

// Reconstruct world position from depth and UV
inline float3 reconstructWorldPosition(float2 uv, float depth, float4x4 invViewProj) {
    // Convert to NDC
    float4 ndc = float4(uv * 2.0 - 1.0, depth, 1.0);
    ndc.y = -ndc.y; // Flip Y for Metal

    // Transform to world space
    float4 worldPos = invViewProj * ndc;
    return worldPos.xyz / worldPos.w;
}

// Reconstruct world position from pixel coordinates
inline float3 reconstructWorldPositionFromPixel(uint2 pixel, float depth,
                                                  float2 screenSize, float4x4 invViewProj) {
    float2 uv = (float2(pixel) + 0.5) / screenSize;
    return reconstructWorldPosition(uv, depth, invViewProj);
}

// ============================================================================
// Surfel Weighting Functions
// ============================================================================

// Distance-based weight for surfel contribution
inline float distanceWeight(float dist, float maxDist) {
    float normalized = saturate(dist / maxDist);
    return 1.0 - normalized * normalized; // Quadratic falloff
}

// Normal similarity weight
inline float normalWeight(float3 n1, float3 n2) {
    float d = dot(n1, n2);
    return saturate(d); // Only positive contributions
}

// Combined surfel weight for sampling
inline float computeSurfelWeight(float3 samplePos, float3 sampleNormal,
                                   Surfel surfel, float maxDistance) {
    float3 toSurfel = surfel.position - samplePos;
    float dist = length(toSurfel);

    if (dist > maxDistance || dist < GIBS_EPSILON) {
        return 0.0;
    }

    float dw = distanceWeight(dist, maxDistance);
    float nw = normalWeight(sampleNormal, surfel.normal);

    return dw * nw;
}

// ============================================================================
// Debug Visualization Helpers
// ============================================================================

// Color-code surfel by flags
inline float3 debugSurfelColor(uint flags) {
    if (flags & SURFEL_FLAG_DYNAMIC) {
        return float3(1.0, 0.5, 0.0); // Orange for dynamic
    } else if (flags & SURFEL_FLAG_STATIC) {
        return float3(0.0, 1.0, 0.5); // Cyan for static
    } else if (flags & SURFEL_FLAG_NEEDS_UPDATE) {
        return float3(1.0, 1.0, 0.0); // Yellow for needs update
    }
    return float3(0.5, 0.5, 0.5); // Gray for invalid
}

// Color-code cell by index
inline float3 debugCellColor(uint cellIndex) {
    return float3(
        float((cellIndex * 73) % 256) / 255.0,
        float((cellIndex * 137) % 256) / 255.0,
        float((cellIndex * 199) % 256) / 255.0
    );
}

#endif // GIBS_COMMON_METAL
