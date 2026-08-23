// GIBS (Global Illumination Based on Surfels) — GLSL twin of gibs_common.metal.
// Struct layouts must match the C++ side (graphics_gibs.hpp) byte-for-byte:
// Metal's packed_float3 + trailing scalar pairs map onto std430 vec3 + scalar
// (vec3 is 16-aligned but 12 bytes, so the following scalar packs into its
// tail slot — same offsets as the packed layout).
#ifndef GIBS_COMMON_GLSL
#define GIBS_COMMON_GLSL

// Surfel flags (matches SurfelFlags enum in graphics.hpp)
const uint SURFEL_FLAG_NONE = 0u;
const uint SURFEL_FLAG_STATIC = 1u << 0;
const uint SURFEL_FLAG_DYNAMIC = 1u << 1;
const uint SURFEL_FLAG_NEEDS_UPDATE = 1u << 2;
const uint SURFEL_FLAG_VALID = 1u << 3;

const float GIBS_PI = 3.14159265359;
const float GIBS_INV_PI = 0.31830988618;
const float GIBS_EPSILON = 1e-6;
const uint GIBS_INVALID_INDEX = 0xFFFFFFFFu;
const uint GIBS_MAX_CHAIN_LENGTH = 64u; // Safety cap when walking cell linked lists

// Single Surfel — 128 bytes (see gibs_common.metal for the field rationale).
struct Surfel {
    vec3 position;    // offset 0
    float radius;     // 12
    vec3 normal;      // 16
    float _pad1;      // 28
    vec3 albedo;      // 32
    float _pad2;      // 44
    vec3 irradiance;  // 48
    float _pad3;      // 60
    vec3 directLight; // 64
    float age;        // 76
    uint cellHash;    // 80
    uint flags;       // 84
    uint instanceID;  // 88
    uint _pad4;       // 92
    vec4 _reserved1;  // 96
    vec4 _reserved2;  // 112
};

// GIBS global parameters — must match GIBSData in gibs_common.metal.
struct GIBSData {
    mat4 invViewProj;
    mat4 prevViewProj;
    vec3 cameraPosition;
    float _pad1;

    vec3 sunDirection;
    float _pad2;
    vec3 sunColor;
    float sunIntensity;

    uint maxSurfels;
    uint activeSurfelCount;
    float surfelRadius;
    float surfelDensity;

    vec3 worldMin;
    float cellSize;
    vec3 worldMax;
    uint totalCells;
    uvec3 gridSize;
    float _pad3;

    uint raysPerSurfel;
    uint maxBounces;
    float rayBias;
    float rayMaxDistance;

    float temporalBlend;
    float hysteresis;
    uint frameIndex;
    float _pad4;

    vec2 screenSize;
    vec2 giResolution;

    uint sampleRadius;
    uint maxSurfelsPerPixel;
    vec2 _pad5;
};

// ============================================================================
// Spatial hash
// ============================================================================

uvec3 worldToCell(vec3 worldPos, vec3 worldMin, float cellSize) {
    vec3 relPos = worldPos - worldMin;
    return uvec3(max(vec3(0.0), floor(relPos / cellSize)));
}

uint cellToIndex(uvec3 cell, uvec3 gridSize) {
    return cell.x + cell.y * gridSize.x + cell.z * gridSize.x * gridSize.y;
}

uint computeCellHash(vec3 worldPos, vec3 worldMin, float cellSize, uvec3 gridSize) {
    uvec3 cell = worldToCell(worldPos, worldMin, cellSize);
    cell = min(cell, gridSize - 1u);
    return cellToIndex(cell, gridSize);
}

bool isInWorldBounds(vec3 pos, vec3 worldMin, vec3 worldMax) {
    return all(greaterThanEqual(pos, worldMin)) && all(lessThanEqual(pos, worldMax));
}

// ============================================================================
// RNG (identical sequences to gibs_common.metal)
// ============================================================================

uint pcgHash(uint x) {
    uint state = x * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float randomFloat(uint seed) {
    return float(pcgHash(seed)) / 4294967295.0;
}

vec2 randomFloat2(uint seed) {
    uint h1 = pcgHash(seed);
    uint h2 = pcgHash(seed + 1u);
    return vec2(float(h1), float(h2)) / 4294967295.0;
}

vec3 randomFloat3(uint seed) {
    uint h1 = pcgHash(seed);
    uint h2 = pcgHash(seed + 1u);
    uint h3 = pcgHash(seed + 2u);
    return vec3(float(h1), float(h2), float(h3)) / 4294967295.0;
}

uint temporalSeed(uvec2 pixel, uint frameIndex) {
    return pixel.x + pixel.y * 65536u + frameIndex * 16777216u;
}

// ============================================================================
// Hemisphere sampling
// ============================================================================

vec3 sampleUniformHemisphere(vec2 u, vec3 normal) {
    float z = u.x;
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = 2.0 * GIBS_PI * u.y;
    vec3 dir = vec3(r * cos(phi), r * sin(phi), z);
    vec3 up = abs(normal.z) < 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    return tangent * dir.x + bitangent * dir.y + normal * dir.z;
}

vec3 sampleCosineHemisphere(vec2 u, vec3 normal) {
    float r = sqrt(u.x);
    float theta = 2.0 * GIBS_PI * u.y;
    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(max(0.0, 1.0 - u.x));
    vec3 up = abs(normal.z) < 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * x + bitangent * y + normal * z);
}

float cosineHemispherePDF(float cosTheta) {
    return cosTheta * GIBS_INV_PI;
}

// ============================================================================
// World reconstruction (identical to gibs_common.metal — its ndc.y = -ndc.y
// flip equals the Vulkan (1 - 2v) convention, so this ports verbatim)
// ============================================================================

vec3 reconstructWorldPosition(vec2 uv, float depth, mat4 invViewProj) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    ndc.y = -ndc.y;
    vec4 worldPos = invViewProj * ndc;
    return worldPos.xyz / worldPos.w;
}

vec3 reconstructWorldPositionFromPixel(uvec2 pixel, float depth,
                                       vec2 screenSize, mat4 invViewProj) {
    vec2 uv = (vec2(pixel) + 0.5) / screenSize;
    return reconstructWorldPosition(uv, depth, invViewProj);
}

// ============================================================================
// Surfel weighting
// ============================================================================

float gibsDistanceWeight(float dist, float maxDist) {
    float normalized = clamp(dist / maxDist, 0.0, 1.0);
    return 1.0 - normalized * normalized; // Quadratic falloff
}

float gibsNormalWeight(vec3 n1, vec3 n2) {
    return clamp(dot(n1, n2), 0.0, 1.0); // Only positive contributions
}

float computeSurfelWeight(vec3 samplePos, vec3 sampleNormal, Surfel surfel, float maxDistance) {
    vec3 toSurfel = surfel.position - samplePos;
    float dist = length(toSurfel);
    if (dist > maxDistance || dist < GIBS_EPSILON) {
        return 0.0;
    }
    float dw = gibsDistanceWeight(dist, maxDistance);
    float nw = gibsNormalWeight(sampleNormal, surfel.normal);
    return dw * nw;
}

// ============================================================================
// Debug visualization
// ============================================================================

vec3 debugSurfelColor(uint flags) {
    if ((flags & SURFEL_FLAG_DYNAMIC) != 0u) {
        return vec3(1.0, 0.5, 0.0);
    } else if ((flags & SURFEL_FLAG_STATIC) != 0u) {
        return vec3(0.0, 1.0, 0.5);
    } else if ((flags & SURFEL_FLAG_NEEDS_UPDATE) != 0u) {
        return vec3(1.0, 1.0, 0.0);
    }
    return vec3(0.5, 0.5, 0.5);
}

vec3 debugCellColor(uint cellIndex) {
    return vec3(
        float((cellIndex * 73u) % 256u) / 255.0,
        float((cellIndex * 137u) % 256u) / 255.0,
        float((cellIndex * 199u) % 256u) / 255.0
    );
}

#endif // GIBS_COMMON_GLSL
