#pragma once
#include <SDL3/SDL_stdinc.h>
#include <cstddef>
#include <glm/glm.hpp>

// ===== GIBS (Global Illumination Based on Surfels) =====
// Design doc: docs/GIBS_DESIGN.md

// Surfel flags for status tracking
enum class SurfelFlags : Uint32 {
    None = 0,
    Static = 1 << 0,      // Static geometry surfel (long lifetime)
    Dynamic = 1 << 1,     // Dynamic object surfel (updated each frame)
    NeedsUpdate = 1 << 2, // Irradiance needs recalculation
    Valid = 1 << 3,       // Surfel contains valid data
};

inline SurfelFlags operator|(SurfelFlags a, SurfelFlags b) {
    return static_cast<SurfelFlags>(static_cast<Uint32>(a) | static_cast<Uint32>(b));
}

inline SurfelFlags operator&(SurfelFlags a, SurfelFlags b) {
    return static_cast<SurfelFlags>(static_cast<Uint32>(a) & static_cast<Uint32>(b));
}

// Single Surfel data structure (GPU-side) - 128 bytes
// Design Decision: 128 bytes chosen for balance between info completeness and memory
// See GIBS_DESIGN.md Decision #1
struct alignas(16) Surfel {
    glm::vec3 position;       // World space position (12 bytes)
    float radius;             // Surfel coverage radius (4 bytes) = 16
    glm::vec3 normal;         // Surface normal (normalized) (12 bytes)
    float _pad1;              // (4 bytes) = 32
    glm::vec3 albedo;         // Surface reflectance (diffuse color) (12 bytes)
    float _pad2;              // (4 bytes) = 48
    glm::vec3 irradiance;     // Accumulated indirect irradiance (RGB) (12 bytes)
    float _pad3;              // (4 bytes) = 64
    glm::vec3 directLight;    // Direct lighting contribution (12 bytes)
    float age;                // Frame age for temporal stability (4 bytes) = 80
    Uint32 cellHash;          // Spatial hash cell ID (4 bytes)
    Uint32 flags;             // SurfelFlags bitmask (4 bytes)
    Uint32 instanceID;        // Source mesh instance (4 bytes)
    Uint32 _pad4;             // (4 bytes) = 96
    glm::vec4 _reserved1;     // Reserved for future use (16 bytes) = 112
    glm::vec4 _reserved2;     // Reserved for future use (16 bytes) = 128
};
static_assert(sizeof(Surfel) == 128, "Surfel must be 128 bytes for GPU alignment");

// Spatial hash cell for fast surfel lookup
// Design Decision: Uniform spatial hash chosen over octree for GPU efficiency
// See GIBS_DESIGN.md Decision #2
struct alignas(16) SurfelCell {
    Uint32 surfelOffset;      // Start index in sorted surfel array
    Uint32 surfelCount;       // Number of surfels in this cell
    Uint32 _pad[2];
};
static_assert(sizeof(SurfelCell) == 16, "SurfelCell must be 16 bytes");

// GIBS global parameters (per-frame uniform buffer)
struct alignas(16) GIBSData {
    // Camera and transform data
    glm::mat4 invViewProj;
    glm::mat4 prevViewProj;       // For temporal reprojection
    glm::vec3 cameraPosition;
    float _pad1;

    // Lighting
    glm::vec3 sunDirection;
    float _pad2;
    glm::vec3 sunColor;
    float sunIntensity;

    // Surfel parameters
    Uint32 maxSurfels;            // Maximum surfel capacity
    Uint32 activeSurfelCount;     // Current active surfel count
    float surfelRadius;           // Default surfel radius
    float surfelDensity;          // Surfels per square meter

    // Spatial hash parameters
    glm::vec3 worldMin;           // World bounds minimum
    float cellSize;               // Hash cell size in world units
    glm::vec3 worldMax;           // World bounds maximum
    Uint32 totalCells;            // Total hash cells
    glm::uvec3 gridSize;          // Hash grid dimensions (X, Y, Z)
    float _pad3;

    // Raytracing parameters
    // Design Decision: 4 rays/surfel with temporal accumulation
    // See GIBS_DESIGN.md Decision #4
    Uint32 raysPerSurfel;         // Rays per surfel per frame (default: 4)
    Uint32 maxBounces;            // Max light bounces (default: 1)
    float rayBias;                // Self-intersection prevention bias
    float rayMaxDistance;         // Maximum ray travel distance

    // Temporal stability parameters
    float temporalBlend;          // Current vs history blend factor (0-1)
    float hysteresis;             // History retention ratio
    Uint32 frameIndex;            // Current frame number
    float _pad4;

    // Screen sampling parameters
    glm::vec2 screenSize;         // Full resolution screen size
    glm::vec2 giResolution;       // GI buffer resolution (typically half)

    // Quality settings
    Uint32 sampleRadius;          // Surfel search radius in cells
    Uint32 maxSurfelsPerPixel;    // Max surfels to sample per pixel
    glm::vec2 _pad5;
};
// These offsets must match the packed_float3-based layout in gibs_common.metal.
// glm::vec3 is 12 bytes / 4-aligned = MSL packed_float3; a non-packed MSL float3
// (16 bytes / 16-aligned) would silently shift everything after cameraPosition.
static_assert(offsetof(GIBSData, cameraPosition) == 128, "GIBSData layout mismatch vs Metal");
static_assert(offsetof(GIBSData, maxSurfels) == 176, "GIBSData layout mismatch vs Metal");
static_assert(offsetof(GIBSData, worldMin) == 192, "GIBSData layout mismatch vs Metal");
static_assert(offsetof(GIBSData, totalCells) == 220, "GIBSData layout mismatch vs Metal");
static_assert(offsetof(GIBSData, rayMaxDistance) == 252, "GIBSData layout mismatch vs Metal");
static_assert(offsetof(GIBSData, sampleRadius) == 288, "GIBSData layout mismatch vs Metal");
static_assert(sizeof(GIBSData) == 304, "GIBSData must be 304 bytes to match Metal");

// GIBS quality presets
enum class GIBSQuality {
    Low,      // 250K surfels, 2 rays, 1/4 res sampling
    Medium,   // 500K surfels, 4 rays, 1/2 res sampling
    High,     // 1M surfels, 8 rays, full res sampling
    Ultra     // 2M surfels, 16 rays, full res sampling
};

// Helper to get quality settings
inline void getGIBSQualitySettings(GIBSQuality quality, Uint32& maxSurfels, Uint32& raysPerSurfel, float& resolutionScale) {
    switch (quality) {
        case GIBSQuality::Low:
            maxSurfels = 250000;
            raysPerSurfel = 2;
            resolutionScale = 0.25f;
            break;
        case GIBSQuality::Medium:
            maxSurfels = 500000;
            raysPerSurfel = 4;
            resolutionScale = 0.5f;
            break;
        case GIBSQuality::High:
            maxSurfels = 1000000;
            raysPerSurfel = 8;
            resolutionScale = 1.0f;
            break;
        case GIBSQuality::Ultra:
            maxSurfels = 2000000;
            raysPerSurfel = 16;
            resolutionScale = 1.0f;
            break;
    }
}

// Surfel generation dispatch parameters
struct alignas(16) SurfelGenerationParams {
    glm::mat4 invViewProj;
    glm::vec2 screenSize;
    float surfelRadius;
    float densityThreshold;       // Probability threshold for surfel creation
    Uint32 maxNewSurfels;         // Max surfels to create this frame
    Uint32 frameIndex;
    glm::vec2 _pad;
};

// Surfel raytracing dispatch parameters
struct alignas(16) SurfelRaytracingParams {
    Uint32 surfelOffset;          // Start index for this dispatch
    Uint32 surfelCount;           // Number of surfels to process
    Uint32 raysPerSurfel;
    Uint32 frameIndex;
    float rayBias;
    float rayMaxDistance;
    glm::vec2 _pad;
};

// Screen-space GI sampling parameters
struct alignas(16) GIBSSampleParams {
    glm::mat4 invViewProj;
    glm::vec2 screenSize;
    glm::vec2 giResolution;
    float sampleRadius;           // World-space search radius
    Uint32 maxSamples;            // Max surfels to sample
    float normalWeight;           // Weight for normal similarity
    float distanceWeight;         // Weight for distance falloff
};
