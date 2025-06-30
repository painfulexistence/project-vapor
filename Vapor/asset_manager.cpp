#include "asset_manager.hpp"

#include <fmt/core.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#define TINYOBJLOADER_IMPLEMENTATION
#include "tinyobjloader/tiny_obj_loader.h"

#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_INCLUDE_STB_IMAGE_WRITE
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

#include "asset_serializer.hpp"
#include "graphics.hpp"

std::shared_ptr<Image> AssetManager::loadImage(const std::string& filename) {
    int width, height, numChannels;
    if (!stbi_info((SDL_GetBasePath() + filename).c_str(), &width, &height, &numChannels)) {
        throw std::runtime_error(fmt::format("Failed to load image at {}!\n", filename));
    }
    int desiredChannels = 0;
    switch (numChannels) {
    case 1:
        desiredChannels = 1;
        break;
    case 3:
        desiredChannels = 4;
        break;
    case 4:
        desiredChannels = 4;
        break;
    default:
        throw std::runtime_error(fmt::format("Unknown texture format at {}\n", filename));
        break;
    }
    uint8_t* data = stbi_load((SDL_GetBasePath() + filename).c_str(), &width, &height, &numChannels, desiredChannels);
    if (data) {
        auto image = std::make_shared<Image>(Image {
            .uri = filename,
            .width = static_cast<Uint32>(width),
            .height = static_cast<Uint32>(height),
            .channelCount = static_cast<Uint32>(desiredChannels),
            .byteArray = std::vector<Uint8>(data, data + width * height * desiredChannels)
        });
        stbi_image_free(data);
        return image;
    } else {
        stbi_image_free(data);
        return nullptr;
    }
}

// TODO: use Scene instead of Mesh
std::shared_ptr<Mesh> AssetManager::loadOBJ(const std::string& filename, const std::string& mtl_basedir) {
    std::vector<VertexData> vertices;
    std::vector<Uint32> indices;

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string err;

    if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &err, (SDL_GetBasePath() + filename).c_str(), mtl_basedir.empty() ? nullptr : (SDL_GetBasePath() + mtl_basedir).c_str())) {
        throw std::runtime_error(fmt::format("Failed to load model: {}", err));
    }

    std::vector<std::shared_ptr<Material>> meshMaterials;
    for (const auto& mat : materials) {
        auto material = std::make_shared<Material>();
        material->name = mat.name;
        if (!mat.diffuse_texname.empty()) {
            material->albedoMap = AssetManager::loadImage(mtl_basedir + mat.diffuse_texname);
        }
        if (!mat.bump_texname.empty()) {
            material->normalMap = AssetManager::loadImage(mtl_basedir + mat.bump_texname);
        }
        if (!mat.metallic_texname.empty()) {
            material->metallicRoughnessMap = AssetManager::loadImage(mtl_basedir + mat.metallic_texname);
        }
        if (!mat.ambient_texname.empty()) {
            // material->occlusionMap = AssetManager::loadImage(mtl_basedir + mat.ambient_texname);
        }
        if (!mat.displacement_texname.empty()) {
            material->displacementMap = AssetManager::loadImage(mtl_basedir + mat.displacement_texname);
        }
        meshMaterials.push_back(material);
    }

    for (const auto& shape : shapes) {
        for (size_t i = 0; i < shape.mesh.indices.size(); i++) {
            auto index = shape.mesh.indices[i];
            auto materialID = -1;
            if (!shape.mesh.material_ids.empty()) {
                materialID = shape.mesh.material_ids[i / 3];
            }

            VertexData vert = {};
            vert.position = { attrib.vertices[3 * index.vertex_index + 0],
                              attrib.vertices[3 * index.vertex_index + 1],
                              attrib.vertices[3 * index.vertex_index + 2] };
            if (attrib.normals.size() > 0) {
                vert.normal = { attrib.normals[3 * index.normal_index + 0],
                                attrib.normals[3 * index.normal_index + 1],
                                attrib.normals[3 * index.normal_index + 2] };
            } else {
                // TODO: calculate normals
            }
            vert.uv = { attrib.texcoords[2 * index.texcoord_index + 0],
                        1.0 - attrib.texcoords[2 * index.texcoord_index + 1] };
            vertices.push_back(vert);
            indices.push_back(indices.size());
        }
    }

    auto mesh = std::make_shared<Mesh>();
    mesh->initialize({ vertices, indices });
    mesh->material = meshMaterials[0];

    return mesh;
}

// std::shared_ptr<Mesh> AssetManager::loadGLTF(const std::string& filename) {
//     std::vector<VertexData> vertices;
//     std::vector<Uint32> indices;

//     cgltf_options options = {};
//     cgltf_data* data = nullptr;

//     cgltf_result result = cgltf_parse_file(&options, (SDL_GetBasePath() + filename).c_str(), &data);
//     if (result != cgltf_result_success) {
//         throw std::runtime_error(fmt::format("Failed to load model at {}!\n", filename));
//     } else {
//         for (cgltf_size i = 0; i < data->meshes_count; ++i) {
//             const cgltf_mesh& mesh = data->meshes[i];
//             for (cgltf_size j = 0; j < mesh.primitives_count; ++j) {
//                 const cgltf_primitive& primitive = mesh.primitives[j];

//                 const cgltf_accessor* position_accessor = nullptr;
//                 const cgltf_accessor* normal_accessor = nullptr;
//                 const cgltf_accessor* uv_accessor = nullptr;
//                 for (cgltf_size k = 0; k < primitive.attributes_count; ++k) {
//                     const cgltf_attribute& attribute = primitive.attributes[k];
//                     if (attribute.type == cgltf_attribute_type_position) {
//                         position_accessor = attribute.data;
//                     } else if (attribute.type == cgltf_attribute_type_normal) {
//                         normal_accessor = attribute.data;
//                     } else if (attribute.type == cgltf_attribute_type_texcoord) {
//                         uv_accessor = attribute.data;
//                     }
//                 }

//                 if (position_accessor) {
//                     for (cgltf_size v = 0; v < position_accessor->count; ++v) {
//                         VertexData vert = {};

//                         cgltf_float position[3];
//                         cgltf_accessor_read_float(position_accessor, v, position, 3);
//                         vert.position = { position[0], position[1], position[2] };
//                         fmt::print("position: {}, {}, {}\n", position[0], position[1], position[2]);
//                         if (normal_accessor && v < normal_accessor->count) {
//                             cgltf_float normal[3];
//                             cgltf_accessor_read_float(normal_accessor, v, normal, 3);
//                             vert.normal = { normal[0], normal[1], normal[2] };
//                         }
//                         if (uv_accessor && v < uv_accessor->count) {
//                             cgltf_float uv[2];
//                             cgltf_accessor_read_float(uv_accessor, v, uv, 2);
//                             vert.uv = { uv[0], uv[1] };
//                         }

//                         vertices.push_back(vert);
//                         indices.push_back(indices.size());
//                     }
//                 }
//             }
//         }
//         cgltf_free(data);
//     }

//     auto mesh = std::make_shared<Mesh>();
//     mesh->initialize({ vertices, indices });
//     fmt::print("vertices size: {}, indices size: {}\n", vertices.size(), indices.size());
//     return mesh;
// }
std::shared_ptr<Scene> AssetManager::loadGLTF(const std::string& filename) {
    std::filesystem::path filePath(SDL_GetBasePath() + filename);
    std::filesystem::path scenePath(filePath); // make a copy
    if (std::filesystem::exists(scenePath.replace_extension(".vscene"))) {
        return AssetSerializer::deserializeScene(scenePath.string());
    }

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;

    bool result = loader.LoadASCIIFromFile(&model, &err, &warn, filePath.c_str());
    if (!warn.empty()) {
        fmt::print("GLTF Warning: {}\n", warn);
    }
    if (!err.empty()) {
        fmt::print("GLTF Error: {}\n", err);
    }
    if (!result) {
        fmt::print("Failed to parse glTF\n");
        return nullptr;
    }

    if (model.scenes.empty()) {
        fmt::print("No scenes found in gltf\n");
        return nullptr;
    }

    std::shared_ptr<Scene> scene = std::make_shared<Scene>();

    const auto GetLocalMatrix = [](const tinygltf::Node &node) -> glm::mat4{
        if (!node.matrix.empty()) {
            return glm::mat4(
                node.matrix[0], node.matrix[1], node.matrix[2], node.matrix[3],
                node.matrix[4], node.matrix[5], node.matrix[6], node.matrix[7],
                node.matrix[8], node.matrix[9], node.matrix[10], node.matrix[11],
                node.matrix[12], node.matrix[13], node.matrix[14], node.matrix[15]
            );
        }
        const auto translation =
            node.translation.empty()
            ? glm::mat4(1.0f)
            : glm::translate(glm::mat4(1.0f), glm::vec3(node.translation[0], node.translation[1], node.translation[2]));;
        const auto rotationQuat =
            node.rotation.empty()
            ? glm::quat(1, 0, 0, 0)
            : glm::quat(float(node.rotation[3]), float(node.rotation[0]), float(node.rotation[1]),float(node.rotation[2]));
        const auto TR = translation * glm::mat4_cast(rotationQuat);
        return node.scale.empty()
            ? TR
            : glm::scale(TR, glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
    };

    // Load images
    std::vector<std::shared_ptr<Image>> images;
    images.reserve(model.images.size());
    for (const auto& img : model.images) {
        images.push_back(std::make_shared<Image>(Image {
            .uri = img.uri,
            .width = static_cast<Uint32>(img.width),
            .height = static_cast<Uint32>(img.height),
            .channelCount = static_cast<Uint32>(img.component),
            .byteArray = std::vector<Uint8>(img.image.begin(), img.image.end())
        }));
    }

    // Load materials
    std::vector<std::shared_ptr<Material>> materials;
    materials.reserve(model.materials.size());
    for (const auto& mat : model.materials) {
        auto material = std::make_shared<Material>();
        material->name = mat.name;
        if (mat.alphaMode == "BLEND") {
            material->alphaMode = AlphaMode::BLEND;
        } else if (mat.alphaMode == "MASK") {
            material->alphaMode = AlphaMode::MASK;
        } else {
            material->alphaMode = AlphaMode::OPAQUE;
        }
        material->alphaCutoff = mat.alphaCutoff;
        material->doubleSided = mat.doubleSided;
        material->baseColorFactor = glm::vec4(mat.pbrMetallicRoughness.baseColorFactor[0], mat.pbrMetallicRoughness.baseColorFactor[1], mat.pbrMetallicRoughness.baseColorFactor[2], mat.pbrMetallicRoughness.baseColorFactor[3]);
        material->metallicFactor = mat.pbrMetallicRoughness.metallicFactor;
        material->roughnessFactor = mat.pbrMetallicRoughness.roughnessFactor;
        material->emissiveFactor = glm::vec3(mat.emissiveFactor[0], mat.emissiveFactor[1], mat.emissiveFactor[2]);
        material->normalScale = mat.normalTexture.scale;
        material->occlusionStrength = mat.occlusionTexture.strength;
        if (mat.pbrMetallicRoughness.baseColorTexture.index >= 0) {
            const auto& texture = model.textures[mat.pbrMetallicRoughness.baseColorTexture.index];
            if (texture.source >= 0) {
                material->albedoMap = images[texture.source];
                // material->uvs["albedo"] = mat.pbrMetallicRoughness.baseColorTexture.texCoord;
                // material->samplers["albedo"] = texture.sampler;
            }
        }
        if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
            const auto& texture = model.textures[mat.pbrMetallicRoughness.metallicRoughnessTexture.index];
            if (texture.source >= 0) {
                material->metallicRoughnessMap = images[texture.source];
                // material->uvs["metallicRoughness"] = mat.pbrMetallicRoughness.metallicRoughnessTexture.texCoord;
                // material->samplers["metallicRoughness"] = texture.sampler;
            }
        }
        if (mat.normalTexture.index >= 0) {
            const auto& texture = model.textures[mat.normalTexture.index];
            if (texture.source >= 0) {
                material->normalMap = images[texture.source];
            }
        }
        if (mat.occlusionTexture.index >= 0) {
            const auto& texture = model.textures[mat.occlusionTexture.index];
            if (texture.source >= 0) {
                material->occlusionMap = images[texture.source];
            }
        }
        if (mat.emissiveTexture.index >= 0) {
            const auto& texture = model.textures[mat.emissiveTexture.index];
            if (texture.source >= 0) {
                material->emissiveMap = images[texture.source];
            }
        }

        materials.push_back(material);
    }

    // Load meshes
    std::vector<std::shared_ptr<MeshGroup>> meshGroups;
    meshGroups.reserve(model.meshes.size());
    for (const auto& srcMesh : model.meshes) {
        auto meshGroup = std::make_shared<MeshGroup>();
        meshGroup->name = srcMesh.name;

        for (const auto& primitive : srcMesh.primitives) {
            bool invalid = false;
            auto mesh = std::make_shared<Mesh>();
            mesh->hasPosition = primitive.attributes.contains("POSITION");
            mesh->hasNormal = primitive.attributes.contains("NORMAL");
            mesh->hasTangent = primitive.attributes.contains("TANGENT");
            mesh->hasUV0 = primitive.attributes.contains("TEXCOORD_0");
            mesh->hasUV1 = primitive.attributes.contains("TEXCOORD_1");
            mesh->hasColor = primitive.attributes.contains("COLOR_0");
            if (!mesh->hasPosition) {
                fmt::print("No position attribute found for primitive\n");
                continue;
            }
            Uint32 vertexCount = model.accessors[primitive.attributes.at("POSITION")].count;
            mesh->vertices.resize(vertexCount);
            if (mesh->hasPosition) {
                const auto& accessor = model.accessors[primitive.attributes.at("POSITION")];
                const auto& bufferView = model.bufferViews[accessor.bufferView];
                const auto& buffer = model.buffers[bufferView.buffer];
                const float* data = reinterpret_cast<const float*>(&buffer.data[bufferView.byteOffset + accessor.byteOffset]);
                for (size_t i = 0; i < vertexCount; i++) {
                    mesh->vertices[i].position = glm::vec3(
                        data[i * 3 + 0],
                        data[i * 3 + 1],
                        data[i * 3 + 2]
                    );
                }
                if (accessor.minValues.size() > 0 && accessor.maxValues.size() > 0) {
                    mesh->localAABBMin = glm::vec3(accessor.minValues[0], accessor.minValues[1], accessor.minValues[2]);
                    mesh->localAABBMax = glm::vec3(accessor.maxValues[0], accessor.maxValues[1], accessor.maxValues[2]);
                } else {
                    mesh->calculateLocalAABB();
                }
                mesh->isGeometryDirty = false;
            }
            if (mesh->hasNormal) {
                const auto& accessor = model.accessors[primitive.attributes.at("NORMAL")];
                const auto& bufferView = model.bufferViews[accessor.bufferView];
                const auto& buffer = model.buffers[bufferView.buffer];
                const float* data = reinterpret_cast<const float*>(&buffer.data[bufferView.byteOffset + accessor.byteOffset]);
                for (size_t i = 0; i < vertexCount; i++) {
                    mesh->vertices[i].normal = glm::vec3(
                        data[i * 3 + 0],
                        data[i * 3 + 1],
                        data[i * 3 + 2]
                    );
                }
            }
            if (mesh->hasTangent) {
                const auto& accessor = model.accessors[primitive.attributes.at("TANGENT")];
                const auto& bufferView = model.bufferViews[accessor.bufferView];
                const auto& buffer = model.buffers[bufferView.buffer];
                const float* data = reinterpret_cast<const float*>(&buffer.data[bufferView.byteOffset + accessor.byteOffset]);
                for (size_t i = 0; i < vertexCount; i++) {
                    mesh->vertices[i].tangent = glm::vec4(
                        data[i * 4 + 0],
                        data[i * 4 + 1],
                        data[i * 4 + 2],
                        data[i * 4 + 3]
                    );
                }
            }
            if (mesh->hasUV0) {
                const auto& accessor = model.accessors[primitive.attributes.at("TEXCOORD_0")];
                const auto& bufferView = model.bufferViews[accessor.bufferView];
                const auto& buffer = model.buffers[bufferView.buffer];
                const float* data = reinterpret_cast<const float*>(&buffer.data[bufferView.byteOffset + accessor.byteOffset]);
                for (size_t i = 0; i < vertexCount; i++) {
                    mesh->vertices[i].uv = glm::vec2(
                        data[i * 2 + 0],
                        data[i * 2 + 1]
                    );
                }
            }
            if (mesh->hasColor) {
                // const auto& accessor = model.accessors[primitive.attributes.at("COLOR_0")];
                // const auto& bufferView = model.bufferViews[accessor.bufferView];
                // const auto& buffer = model.buffers[bufferView.buffer];
                // const float* data = reinterpret_cast<const float*>(&buffer.data[bufferView.byteOffset + accessor.byteOffset]);
                // for (size_t i = 0; i < vertexCount; i++) {
                //     mesh->vertices[i].color = glm::vec4(
                //         data[i * 4 + 0],
                //         data[i * 4 + 1],
                //         data[i * 4 + 2],
                //         data[i * 4 + 3]
                //     );
                // }
            }
            if (primitive.indices >= 0) {
                const auto& accessor = model.accessors[primitive.indices];
                const auto& bufferView = model.bufferViews[accessor.bufferView];
                const auto& buffer = model.buffers[bufferView.buffer];

                mesh->indices.resize(accessor.count);

                switch (accessor.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                    const Uint16* data = reinterpret_cast<const Uint16*>(
                        &buffer.data[bufferView.byteOffset + accessor.byteOffset]
                    );
                    for (size_t i = 0; i < accessor.count; i++) {
                        mesh->indices[i] = static_cast<Uint32>(data[i]);
                    }
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                    const Uint32* data = reinterpret_cast<const Uint32*>(
                        &buffer.data[bufferView.byteOffset + accessor.byteOffset]
                    );
                    for (size_t i = 0; i < accessor.count; i++) {
                        mesh->indices[i] = data[i];
                    }
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
                    const Uint8* data = reinterpret_cast<const Uint8*>(
                        &buffer.data[bufferView.byteOffset + accessor.byteOffset]
                    );
                    for (size_t i = 0; i < accessor.count; i++) {
                        mesh->indices[i] = static_cast<Uint32>(data[i]);
                    }
                    break;
                }
                default:
                    fmt::print("Unsupported index component type: {}\n", accessor.componentType);
                    break;
                }
            }
            if (primitive.material >= 0) {
                mesh->material = materials[primitive.material];
            } else {
                // if no material is specified, mesh->material would be nullptr
                fmt::print("No material specified for primitive\n");
            }
            switch (primitive.mode) {
            case TINYGLTF_MODE_POINTS:
                mesh->primitiveMode = PrimitiveMode::POINTS;
                break;
            case TINYGLTF_MODE_LINE:
                mesh->primitiveMode = PrimitiveMode::LINES;
                break;
            case TINYGLTF_MODE_LINE_STRIP:
                mesh->primitiveMode = PrimitiveMode::LINE_STRIP;
                break;
            case TINYGLTF_MODE_TRIANGLES:
                mesh->primitiveMode = PrimitiveMode::TRIANGLES;
                break;
            case TINYGLTF_MODE_TRIANGLE_STRIP:
                mesh->primitiveMode = PrimitiveMode::TRIANGLE_STRIP;
                break;
            default:
                throw std::runtime_error("Unsupported primitive mode");
            }

            // Fix missing attributes
            if (!mesh->hasNormal) {
                mesh->calculateNormals();
                mesh->hasNormal = true;
            }
            if (!mesh->hasTangent) {
                mesh->calculateTangents();
                mesh->hasTangent = true;
            }

            meshGroup->meshes.push_back(mesh);
        }

        meshGroups.push_back(meshGroup);
    }

    std::function<std::shared_ptr<Node>(int)> createNode = [&](int nodeIndex) -> std::shared_ptr<Node> {
        const auto& srcNode = model.nodes[nodeIndex];
        auto node = std::make_shared<Node>();
        node->name = srcNode.name;
        node->localTransform = GetLocalMatrix(srcNode);

        if (srcNode.mesh >= 0) {
            node->meshGroup = meshGroups[srcNode.mesh];
        }
        for (int childIdx : srcNode.children) {
            node->children.push_back(createNode(childIdx));
        }

        return node;
    };

    const auto& srcScene = model.defaultScene >= 0 ? model.scenes[model.defaultScene] : model.scenes[0];
    scene->name = srcScene.name;
    // TODO: maybe directly store the images and materials in the scene
    scene->images = std::move(images);
    scene->materials = std::move(materials);
    for (int nodeIdx : srcScene.nodes) {
        scene->nodes.push_back(createNode(nodeIdx));
    }
    scene->update(0.0f); // making sure world transform is updated

    AssetSerializer::serializeScene(scene, scenePath.string());

    return scene;
}

std::shared_ptr<Scene> AssetManager::loadGLTFOptimized(const std::string& filename) {
    std::filesystem::path filePath(SDL_GetBasePath() + filename);
    std::filesystem::path scenePath(filePath); // make a copy
    if (std::filesystem::exists(scenePath.replace_extension(".vscene_optimized"))) {
        return AssetSerializer::deserializeScene(scenePath.string());
    }

    auto originalScene = loadGLTF(filename);
    if (!originalScene) {
        throw std::runtime_error(fmt::format("Failed to load GLTF: {}", filename));
    }

    std::shared_ptr<Scene> optimizedScene = std::make_shared<Scene>();

    optimizedScene->name = originalScene->name;
    optimizedScene->materials = originalScene->materials;
    optimizedScene->images = originalScene->images;
    optimizedScene->directionalLights = originalScene->directionalLights;
    optimizedScene->pointLights = originalScene->pointLights;

    Uint32 totalVertexCount = 0;
    Uint32 totalIndexCount = 0;
    std::function<void(const std::shared_ptr<Node>&)> countNode = [&](const std::shared_ptr<Node>& node) {
        if (node->meshGroup) {
            for (const auto& mesh : node->meshGroup->meshes) {
                totalVertexCount += mesh->vertices.size();
                totalIndexCount += mesh->indices.size();
            }
        }
        for (const auto& child : node->children) {
            countNode(child);
        }
    };
    for (const auto& node : originalScene->nodes) {
        countNode(node);
    }
    optimizedScene->vertices.reserve(totalVertexCount);
    optimizedScene->indices.reserve(totalIndexCount);

    Uint32 currentVertexOffset = 0;
    Uint32 currentIndexOffset = 0;
    std::function<std::shared_ptr<Node>(const std::shared_ptr<Node>&)> processNode = [&](const std::shared_ptr<Node>& originalNode) -> std::shared_ptr<Node> {
        auto newNode = std::make_shared<Node>();
        newNode->name = originalNode->name;
        newNode->localTransform = originalNode->localTransform;
        if (originalNode->meshGroup) {
            auto newMeshGroup = std::make_shared<MeshGroup>();
            newMeshGroup->name = originalNode->meshGroup->name;

            for (const auto& originalMesh : originalNode->meshGroup->meshes) {
                auto newMesh = std::make_shared<Mesh>();
                newMesh->hasPosition = originalMesh->hasPosition;
                newMesh->hasNormal = originalMesh->hasNormal;
                newMesh->hasTangent = originalMesh->hasTangent;
                newMesh->hasUV0 = originalMesh->hasUV0;
                newMesh->hasUV1 = originalMesh->hasUV1;
                newMesh->hasColor = originalMesh->hasColor;
                newMesh->material = originalMesh->material;
                newMesh->primitiveMode = originalMesh->primitiveMode;
                newMesh->localAABBMin = originalMesh->localAABBMin;
                newMesh->localAABBMax = originalMesh->localAABBMax;
                newMesh->vertexOffset = currentVertexOffset;
                newMesh->indexOffset = currentIndexOffset;
                newMesh->vertexCount = originalMesh->vertices.size();
                newMesh->indexCount = originalMesh->indices.size();

                optimizedScene->vertices.insert(
                    optimizedScene->vertices.end(),
                    originalMesh->vertices.begin(),
                    originalMesh->vertices.end()
                );
                for (Uint32 index : originalMesh->indices) {
                    optimizedScene->indices.push_back(index);
                }
                currentVertexOffset += originalMesh->vertices.size();
                currentIndexOffset += originalMesh->indices.size();
                newMeshGroup->meshes.push_back(newMesh);

                fmt::print("Mesh: vertexOffset={}, indexOffset={}, vertexCount={}, indexCount={}\n",
                    newMesh->vertexOffset, newMesh->indexOffset, newMesh->vertexCount, newMesh->indexCount);

                // TODO: clean up
                // originalMesh->vertices.clear();
                // originalMesh->indices.clear();
            }
            newNode->meshGroup = newMeshGroup;
        }
        for (const auto& originalChild : originalNode->children) {
            auto newChild = processNode(originalChild);
            newNode->children.push_back(newChild);
        }
        return newNode;
    };
    for (const auto& originalNode : originalScene->nodes) {
        auto newNode = processNode(originalNode);
        optimizedScene->nodes.push_back(newNode);
    }

    fmt::print("Optimized scene created: {} vertices, {} indices\n",
               optimizedScene->vertices.size(), optimizedScene->indices.size());

    optimizedScene->update(0.0f); // making sure world transform is updated

    AssetSerializer::serializeScene(optimizedScene, scenePath.string());

    return optimizedScene;
}