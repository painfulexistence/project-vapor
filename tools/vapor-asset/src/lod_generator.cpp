#include "lod_generator.hpp"
#include <meshoptimizer.h>
#include <fmt/core.h>
#include <chrono>
#include <algorithm>
#include <cmath>

namespace vapor_asset {

LODMesh LODGenerator::generateLODs(
    const std::vector<VertexData>& vertices,
    const std::vector<uint32_t>& indices,
    const LODConfig& config
) {
    auto startTime = std::chrono::high_resolution_clock::now();

    LODMesh result;
    m_stats = Stats{};
    m_stats.originalTriangles = static_cast<uint32_t>(indices.size() / 3);

    // Calculate bounds
    calculateBounds(result, vertices);

    // LOD 0: Original mesh (optimized)
    {
        LODLevel lod0;
        lod0.vertices = vertices;
        lod0.indices = indices;
        lod0.error = 0.0f;
        lod0.screenSizeThreshold = config.screenSizeThresholds.empty() ? 0.1f : config.screenSizeThresholds[0];

        // Optimize vertex cache for better GPU performance
        std::vector<uint32_t> optimizedIndices(indices.size());
        meshopt_optimizeVertexCache(
            optimizedIndices.data(),
            indices.data(),
            indices.size(),
            vertices.size()
        );
        lod0.indices = std::move(optimizedIndices);

        result.lodLevels.push_back(std::move(lod0));
        m_stats.trianglesPerLOD.push_back(m_stats.originalTriangles);
        m_stats.reductionPerLOD.push_back(1.0f);
    }

    // Generate subsequent LOD levels
    std::vector<uint32_t> currentIndices = result.lodLevels[0].indices;
    size_t currentIndexCount = currentIndices.size();

    for (uint32_t lodLevel = 1; lodLevel < config.maxLODLevels; ++lodLevel) {
        // Target index count for this LOD
        size_t targetIndexCount = static_cast<size_t>(
            currentIndexCount * config.targetReductionPerLevel
        );

        // Minimum reasonable triangle count
        if (targetIndexCount < 36) { // 12 triangles minimum
            break;
        }

        // Simplify mesh
        float targetError = config.errorThreshold * static_cast<float>(lodLevel);
        float resultError = 0.0f;

        std::vector<uint32_t> simplifiedIndices(currentIndices.size());
        unsigned int options = 0;
        if (config.lockBorders) {
            options |= meshopt_SimplifyLockBorder;
        }

        size_t newIndexCount = meshopt_simplify(
            simplifiedIndices.data(),
            currentIndices.data(),
            currentIndices.size(),
            reinterpret_cast<const float*>(vertices.data()),
            vertices.size(),
            sizeof(VertexData),
            targetIndexCount,
            targetError,
            options,
            &resultError
        );

        // Check if simplification made meaningful progress
        if (newIndexCount >= currentIndexCount * 0.95f) {
            // Try sloppy simplification for more aggressive reduction
            newIndexCount = meshopt_simplifySloppy(
                simplifiedIndices.data(),
                currentIndices.data(),
                currentIndices.size(),
                reinterpret_cast<const float*>(vertices.data()),
                vertices.size(),
                sizeof(VertexData),
                targetIndexCount,
                targetError * 2.0f,
                &resultError
            );

            if (newIndexCount >= currentIndexCount * 0.95f) {
                // Cannot simplify further
                break;
            }
        }

        simplifiedIndices.resize(newIndexCount);

        // Optimize the simplified mesh
        meshopt_optimizeVertexCache(
            simplifiedIndices.data(),
            simplifiedIndices.data(),
            simplifiedIndices.size(),
            vertices.size()
        );

        // Create LOD level
        LODLevel lod;
        lod.vertices = vertices; // Share vertex data, indices point to subset
        lod.indices = std::move(simplifiedIndices);
        lod.error = resultError;
        lod.screenSizeThreshold = (lodLevel < config.screenSizeThresholds.size())
            ? config.screenSizeThresholds[lodLevel]
            : 0.0f;

        uint32_t triangleCount = static_cast<uint32_t>(lod.indices.size() / 3);
        m_stats.trianglesPerLOD.push_back(triangleCount);
        m_stats.reductionPerLOD.push_back(
            static_cast<float>(triangleCount) / static_cast<float>(m_stats.originalTriangles)
        );

        result.lodLevels.push_back(std::move(lod));

        // Use this LOD as base for next simplification
        currentIndices = result.lodLevels.back().indices;
        currentIndexCount = currentIndices.size();
    }

    // Calculate total triangles
    for (const auto& lod : result.lodLevels) {
        m_stats.totalTrianglesAllLODs += static_cast<uint32_t>(lod.indices.size() / 3);
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    m_stats.processingTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

    return result;
}

void LODGenerator::calculateBounds(LODMesh& mesh, const std::vector<VertexData>& vertices) {
    if (vertices.empty()) {
        mesh.localAABBMin = glm::vec3(0.0f);
        mesh.localAABBMax = glm::vec3(0.0f);
        mesh.boundingSphereCenter = glm::vec3(0.0f);
        mesh.boundingSphereRadius = 0.0f;
        return;
    }

    // Calculate AABB
    mesh.localAABBMin = vertices[0].position;
    mesh.localAABBMax = vertices[0].position;

    for (const auto& v : vertices) {
        mesh.localAABBMin = glm::min(mesh.localAABBMin, v.position);
        mesh.localAABBMax = glm::max(mesh.localAABBMax, v.position);
    }

    // Calculate bounding sphere (simple method: sphere around AABB center)
    mesh.boundingSphereCenter = (mesh.localAABBMin + mesh.localAABBMax) * 0.5f;
    mesh.boundingSphereRadius = 0.0f;

    for (const auto& v : vertices) {
        float dist = glm::length(v.position - mesh.boundingSphereCenter);
        mesh.boundingSphereRadius = std::max(mesh.boundingSphereRadius, dist);
    }
}

} // namespace vapor_asset
