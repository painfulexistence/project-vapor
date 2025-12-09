#include "gltf_loader.hpp"
#include <fmt/core.h>

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

namespace vapor_asset {

namespace {

glm::mat4 getNodeTransform(const tinygltf::Node& node) {
    glm::mat4 transform(1.0f);

    if (!node.matrix.empty()) {
        // Use matrix directly
        for (int i = 0; i < 16; ++i) {
            transform[i / 4][i % 4] = static_cast<float>(node.matrix[i]);
        }
    } else {
        // Build from TRS
        glm::vec3 translation(0.0f);
        glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale(1.0f);

        if (!node.translation.empty()) {
            translation = glm::vec3(
                static_cast<float>(node.translation[0]),
                static_cast<float>(node.translation[1]),
                static_cast<float>(node.translation[2])
            );
        }
        if (!node.rotation.empty()) {
            rotation = glm::quat(
                static_cast<float>(node.rotation[3]), // w
                static_cast<float>(node.rotation[0]), // x
                static_cast<float>(node.rotation[1]), // y
                static_cast<float>(node.rotation[2])  // z
            );
        }
        if (!node.scale.empty()) {
            scale = glm::vec3(
                static_cast<float>(node.scale[0]),
                static_cast<float>(node.scale[1]),
                static_cast<float>(node.scale[2])
            );
        }

        glm::mat4 T = glm::translate(glm::mat4(1.0f), translation);
        glm::mat4 R = glm::mat4_cast(rotation);
        glm::mat4 S = glm::scale(glm::mat4(1.0f), scale);
        transform = T * R * S;
    }

    return transform;
}

template<typename T>
std::vector<T> readAccessorData(const tinygltf::Model& model, int accessorIndex) {
    if (accessorIndex < 0) {
        return {};
    }

    const auto& accessor = model.accessors[accessorIndex];
    const auto& bufferView = model.bufferViews[accessor.bufferView];
    const auto& buffer = model.buffers[bufferView.buffer];

    const uint8_t* data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
    size_t stride = bufferView.byteStride ? bufferView.byteStride : sizeof(T);

    std::vector<T> result(accessor.count);
    for (size_t i = 0; i < accessor.count; ++i) {
        result[i] = *reinterpret_cast<const T*>(data + i * stride);
    }

    return result;
}

std::vector<uint32_t> readIndices(const tinygltf::Model& model, int accessorIndex) {
    if (accessorIndex < 0) {
        return {};
    }

    const auto& accessor = model.accessors[accessorIndex];
    const auto& bufferView = model.bufferViews[accessor.bufferView];
    const auto& buffer = model.buffers[bufferView.buffer];

    const uint8_t* data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;

    std::vector<uint32_t> result(accessor.count);

    switch (accessor.componentType) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            for (size_t i = 0; i < accessor.count; ++i) {
                result[i] = static_cast<uint32_t>(data[i]);
            }
            break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            for (size_t i = 0; i < accessor.count; ++i) {
                result[i] = static_cast<uint32_t>(reinterpret_cast<const uint16_t*>(data)[i]);
            }
            break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            for (size_t i = 0; i < accessor.count; ++i) {
                result[i] = reinterpret_cast<const uint32_t*>(data)[i];
            }
            break;
        default:
            fmt::print("Warning: Unsupported index component type: {}\n", accessor.componentType);
            break;
    }

    return result;
}

} // anonymous namespace

SceneData GLTFLoader::loadAndGenerateLODs(
    const std::string& filepath,
    const LODConfig& config
) {
    SceneData scene;

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    bool success = false;
    if (filepath.ends_with(".glb")) {
        success = loader.LoadBinaryFromFile(&model, &err, &warn, filepath);
    } else {
        success = loader.LoadASCIIFromFile(&model, &err, &warn, filepath);
    }

    if (!warn.empty()) {
        fmt::print("Warning: {}\n", warn);
    }
    if (!err.empty()) {
        fmt::print("Error: {}\n", err);
    }
    if (!success) {
        throw std::runtime_error(fmt::format("Failed to load GLTF: {}", filepath));
    }

    scene.name = filepath;

    // Load materials
    for (const auto& mat : model.materials) {
        MaterialData material;
        material.name = mat.name;

        const auto& pbr = mat.pbrMetallicRoughness;
        material.baseColorFactor = glm::vec4(
            static_cast<float>(pbr.baseColorFactor[0]),
            static_cast<float>(pbr.baseColorFactor[1]),
            static_cast<float>(pbr.baseColorFactor[2]),
            static_cast<float>(pbr.baseColorFactor[3])
        );
        material.metallicFactor = static_cast<float>(pbr.metallicFactor);
        material.roughnessFactor = static_cast<float>(pbr.roughnessFactor);

        scene.materials.push_back(std::move(material));
    }

    // Ensure at least one default material
    if (scene.materials.empty()) {
        MaterialData defaultMat;
        defaultMat.name = "default";
        scene.materials.push_back(defaultMat);
    }

    // Load and process meshes
    fmt::print("Processing {} meshes...\n", model.meshes.size());

    for (size_t meshIdx = 0; meshIdx < model.meshes.size(); ++meshIdx) {
        const auto& gltfMesh = model.meshes[meshIdx];

        for (size_t primIdx = 0; primIdx < gltfMesh.primitives.size(); ++primIdx) {
            const auto& primitive = gltfMesh.primitives[primIdx];

            if (primitive.mode != TINYGLTF_MODE_TRIANGLES) {
                fmt::print("Skipping non-triangle primitive in mesh '{}'\n", gltfMesh.name);
                continue;
            }

            // Read vertex attributes
            std::vector<glm::vec3> positions;
            std::vector<glm::vec2> uvs;
            std::vector<glm::vec3> normals;
            std::vector<glm::vec4> tangents;

            auto posIt = primitive.attributes.find("POSITION");
            if (posIt != primitive.attributes.end()) {
                positions = readAccessorData<glm::vec3>(model, posIt->second);
            }

            auto uvIt = primitive.attributes.find("TEXCOORD_0");
            if (uvIt != primitive.attributes.end()) {
                uvs = readAccessorData<glm::vec2>(model, uvIt->second);
            }

            auto normIt = primitive.attributes.find("NORMAL");
            if (normIt != primitive.attributes.end()) {
                normals = readAccessorData<glm::vec3>(model, normIt->second);
            }

            auto tanIt = primitive.attributes.find("TANGENT");
            if (tanIt != primitive.attributes.end()) {
                tangents = readAccessorData<glm::vec4>(model, tanIt->second);
            }

            // Read indices
            std::vector<uint32_t> indices = readIndices(model, primitive.indices);

            if (positions.empty() || indices.empty()) {
                fmt::print("Skipping empty primitive in mesh '{}'\n", gltfMesh.name);
                continue;
            }

            // Build interleaved vertex data
            std::vector<VertexData> vertices(positions.size());
            for (size_t i = 0; i < positions.size(); ++i) {
                vertices[i].position = positions[i];
                vertices[i].uv = i < uvs.size() ? uvs[i] : glm::vec2(0.0f);
                vertices[i].normal = i < normals.size() ? normals[i] : glm::vec3(0.0f, 1.0f, 0.0f);
                vertices[i].tangent = i < tangents.size() ? tangents[i] : glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            }

            // Generate LODs
            uint32_t originalTriangles = static_cast<uint32_t>(indices.size() / 3);
            scene.totalOriginalTriangles += originalTriangles;

            fmt::print("  Mesh '{}' primitive {}: {} triangles -> ",
                gltfMesh.name, primIdx, originalTriangles);

            LODMesh lodMesh = m_lodGenerator.generateLODs(vertices, indices, config);
            lodMesh.name = gltfMesh.name + "_" + std::to_string(primIdx);
            lodMesh.materialIndex = (primitive.material >= 0)
                ? static_cast<uint32_t>(primitive.material)
                : 0;

            const auto& stats = m_lodGenerator.getStats();
            fmt::print("{} LODs (", lodMesh.lodLevels.size());
            for (size_t i = 0; i < stats.trianglesPerLOD.size(); ++i) {
                fmt::print("{}", stats.trianglesPerLOD[i]);
                if (i < stats.trianglesPerLOD.size() - 1) fmt::print(", ");
            }
            fmt::print(") in {:.1f}ms\n", stats.processingTimeMs);

            scene.totalTrianglesWithLODs += stats.totalTrianglesAllLODs;
            scene.meshes.push_back(std::move(lodMesh));
        }
    }

    // Build scene hierarchy
    std::function<std::shared_ptr<SceneNode>(int)> processNode =
        [&](int nodeIdx) -> std::shared_ptr<SceneNode> {
            const auto& gltfNode = model.nodes[nodeIdx];

            auto node = std::make_shared<SceneNode>();
            node->name = gltfNode.name;
            node->localTransform = getNodeTransform(gltfNode);

            if (gltfNode.mesh >= 0) {
                // Find mesh indices in our processed meshes
                // Note: Each GLTF mesh can have multiple primitives, we processed them separately
                const auto& gltfMesh = model.meshes[gltfNode.mesh];
                uint32_t baseIndex = 0;
                for (int m = 0; m < gltfNode.mesh; ++m) {
                    baseIndex += static_cast<uint32_t>(model.meshes[m].primitives.size());
                }
                for (size_t p = 0; p < gltfMesh.primitives.size(); ++p) {
                    node->meshIndices.push_back(baseIndex + static_cast<uint32_t>(p));
                }
            }

            for (int childIdx : gltfNode.children) {
                node->children.push_back(processNode(childIdx));
            }

            return node;
        };

    // Process root nodes
    if (!model.scenes.empty()) {
        const auto& gltfScene = model.scenes[model.defaultScene >= 0 ? model.defaultScene : 0];
        for (int rootIdx : gltfScene.nodes) {
            scene.rootNodes.push_back(processNode(rootIdx));
        }
    }

    fmt::print("\nTotal: {} original triangles, {} triangles across all LODs\n",
        scene.totalOriginalTriangles, scene.totalTrianglesWithLODs);

    return scene;
}

SceneData GLTFLoader::loadWithoutLODs(const std::string& filepath) {
    LODConfig config;
    config.maxLODLevels = 1; // Only LOD0
    return loadAndGenerateLODs(filepath, config);
}

} // namespace vapor_asset
