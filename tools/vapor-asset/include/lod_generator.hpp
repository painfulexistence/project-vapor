#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vector>
#include <cstdint>
#include <string>

namespace vapor_asset {

// Matches Vapor's VertexData structure
struct VertexData {
    glm::vec3 position;
    glm::vec2 uv;
    glm::vec3 normal;
    glm::vec4 tangent;
};

// Single LOD level data
struct LODLevel {
    std::vector<VertexData> vertices;
    std::vector<uint32_t> indices;
    float error;           // Screen-space error threshold for this LOD
    float screenSizeThreshold; // Minimum screen size to use this LOD
};

// Mesh with multiple LOD levels
struct LODMesh {
    std::string name;
    std::vector<LODLevel> lodLevels;  // LOD0 = highest detail, LODN = lowest
    glm::vec3 localAABBMin;
    glm::vec3 localAABBMax;
    glm::vec3 boundingSphereCenter;
    float boundingSphereRadius;
    uint32_t materialIndex;
};

// Configuration for LOD generation
struct LODConfig {
    uint32_t maxLODLevels = 5;          // Maximum number of LOD levels (including LOD0)
    float targetReductionPerLevel = 0.5f; // Target triangle reduction per level (0.5 = 50%)
    float errorThreshold = 0.01f;       // Maximum simplification error
    bool lockBorders = true;            // Lock mesh border vertices during simplification
    bool preserveAttributes = true;     // Try to preserve UV seams and hard edges

    // Screen-size thresholds for LOD switching (percentage of screen height)
    // Default: LOD0 > 10%, LOD1 > 5%, LOD2 > 2.5%, LOD3 > 1%, LOD4 < 1%
    std::vector<float> screenSizeThresholds = {0.10f, 0.05f, 0.025f, 0.01f, 0.0f};
};

class LODGenerator {
public:
    LODGenerator() = default;
    ~LODGenerator() = default;

    // Generate LOD levels for a single mesh
    LODMesh generateLODs(
        const std::vector<VertexData>& vertices,
        const std::vector<uint32_t>& indices,
        const LODConfig& config = LODConfig{}
    );

    // Get statistics from last generation
    struct Stats {
        uint32_t originalTriangles = 0;
        uint32_t totalTrianglesAllLODs = 0;
        std::vector<uint32_t> trianglesPerLOD;
        std::vector<float> reductionPerLOD;
        float processingTimeMs = 0.0f;
    };
    const Stats& getStats() const { return m_stats; }

private:
    void calculateBounds(LODMesh& mesh, const std::vector<VertexData>& vertices);
    Stats m_stats;
};

} // namespace vapor_asset
