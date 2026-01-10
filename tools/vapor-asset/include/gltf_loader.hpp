#pragma once

#include "lod_generator.hpp"
#include <string>
#include <vector>
#include <memory>
#include <glm/mat4x4.hpp>

namespace vapor_asset {

// Material data (simplified for LOD tool)
struct MaterialData {
    std::string name;
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    std::string albedoTexturePath;
    std::string normalTexturePath;
    std::string metallicRoughnessTexturePath;
};

// Node in scene hierarchy
struct SceneNode {
    std::string name;
    glm::mat4 localTransform;
    std::vector<uint32_t> meshIndices;  // Indices into SceneData::meshes
    std::vector<std::shared_ptr<SceneNode>> children;
};

// Complete scene data loaded from GLTF
struct SceneData {
    std::string name;
    std::vector<LODMesh> meshes;        // Meshes with LOD data
    std::vector<MaterialData> materials;
    std::vector<std::shared_ptr<SceneNode>> rootNodes;

    // Statistics
    uint32_t totalOriginalTriangles = 0;
    uint32_t totalTrianglesWithLODs = 0;
};

class GLTFLoader {
public:
    GLTFLoader() = default;
    ~GLTFLoader() = default;

    // Load GLTF and generate LODs for all meshes
    SceneData loadAndGenerateLODs(
        const std::string& filepath,
        const LODConfig& config = LODConfig{}
    );

    // Load GLTF without LOD generation (for inspection)
    SceneData loadWithoutLODs(const std::string& filepath);

private:
    LODGenerator m_lodGenerator;
};

} // namespace vapor_asset
