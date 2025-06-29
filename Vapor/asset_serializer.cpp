#include "asset_serializer.hpp"
#include <fmt/core.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <fstream>
#include <unordered_map>
#include <stdexcept>


void AssetSerializer::serializeScene(const std::shared_ptr<Scene>& scene, const std::string& path) {
    auto start = SDL_GetTicks();

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error(fmt::format("Failed to open file for writing: {}", path));
    }

    cereal::BinaryOutputArchive archive(file);
    // archive(scene.name);

    std::unordered_map<std::shared_ptr<Image>, Uint32> imageIDs;
    Uint32 nextImageID = 0;
    archive(static_cast<Uint32>(scene->images.size()));
    for (const auto& image : scene->images) {
        if (image && imageIDs.find(image) == imageIDs.end()) {
            archive(nextImageID);
            serializeImage(archive, image);
            imageIDs[image] = nextImageID++;
        }
    }
    fmt::print("Image serialized\n");

    std::unordered_map<std::shared_ptr<Material>, Uint32> materialIDs;
    Uint32 nextMaterialID = 0;
    archive(static_cast<Uint32>(scene->materials.size()));
    for (const auto& material : scene->materials) {
        if (material && materialIDs.find(material) == materialIDs.end()) {
            archive(nextMaterialID);
            serializeMaterial(archive, material, imageIDs);
            materialIDs[material] = nextMaterialID++;
        }
    }
    fmt::print("Material serialized\n");

    archive(static_cast<Uint32>(scene->directionalLights.size()));
    for (const auto& light : scene->directionalLights) {
        serializeDirectionalLight(archive, light);
    }

    archive(static_cast<Uint32>(scene->pointLights.size()));
    for (const auto& light : scene->pointLights) {
        serializePointLight(archive, light);
    }

    archive(static_cast<Uint32>(scene->nodes.size()));
    for (const auto& node : scene->nodes) {
        serializeNode(archive, node, materialIDs);
    }

    fmt::print("Scene serialized to: {} in {} ms\n", path, SDL_GetTicks() - start);
}

std::shared_ptr<Scene> AssetSerializer::deserializeScene(const std::string& path) {
    auto start = SDL_GetTicks();

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error(fmt::format("Failed to open file for reading: {}", path));
    }

    cereal::BinaryInputArchive archive(file);
    auto scene = std::make_shared<Scene>();
    // archive(scene->name);

    Uint32 imageCount;
    archive(imageCount);
    scene->images.reserve(imageCount);
    std::unordered_map<Uint32, std::shared_ptr<Image>> images;
    for (Uint32 i = 0; i < imageCount; ++i) {
        Uint32 imageID;
        archive(imageID);
        auto image = deserializeImage(archive);
        if (image) {
            scene->images.push_back(image);
            images[imageID] = image;
        }
    }

    Uint32 materialCount;
    archive(materialCount);
    scene->materials.reserve(materialCount);
    std::unordered_map<Uint32, std::shared_ptr<Material>> materials;
    for (Uint32 i = 0; i < materialCount; ++i) {
        Uint32 materialID;
        archive(materialID);
        auto material = deserializeMaterial(archive, images);
        if (material) {
            scene->materials.push_back(material);
            materials[materialID] = material;
        }
    }

    Uint32 nodeCount;
    archive(nodeCount);
    scene->nodes.reserve(nodeCount);
    for (Uint32 i = 0; i < nodeCount; ++i) {
        scene->nodes.push_back(deserializeNode(archive, materials));
    }

    Uint32 directionalLightCount;
    archive(directionalLightCount);
    scene->directionalLights.reserve(directionalLightCount);
    for (Uint32 i = 0; i < directionalLightCount; ++i) {
        DirectionalLight light = deserializeDirectionalLight(archive);
        scene->directionalLights.push_back(light);
    }

    Uint32 pointLightCount;
    archive(pointLightCount);
    scene->pointLights.reserve(pointLightCount);
    for (Uint32 i = 0; i < pointLightCount; ++i) {
        PointLight light = deserializePointLight(archive);
        scene->pointLights.push_back(light);
    }

    fmt::print("Scene deserialized from: {} in {} ms\n", path, SDL_GetTicks() - start);
    return scene;
}

void AssetSerializer::serializeNode(cereal::BinaryOutputArchive& archive, const std::shared_ptr<Node>& node,
                                                  const std::unordered_map<std::shared_ptr<Material>, Uint32>& materialIDs) {
    if (!node) {
        archive(false);
        return;
    }
    archive(true);
    // archive(node->name);
    archive(node->localTransform);
    archive(node->worldTransform);
    archive(node->isTransformDirty);
    if (!node->meshGroup) {
        archive(false);
    } else {
        archive(true);
        // archive(node->meshGroup->name);
        archive(static_cast<Uint32>(node->meshGroup->meshes.size()));
        for (const auto& mesh : node->meshGroup->meshes) {
            serializeMesh(archive, mesh, materialIDs);
        }
    }
    archive(static_cast<Uint32>(node->children.size()));
    for (const auto& child : node->children) {
        serializeNode(archive, child, materialIDs);
    }
}

std::shared_ptr<Node> AssetSerializer::deserializeNode(cereal::BinaryInputArchive& archive,
                                                                     const std::unordered_map<Uint32, std::shared_ptr<Material>>& materials) {
    bool isNotNull;
    archive(isNotNull);
    if (!isNotNull) {
        return nullptr;
    }
    auto node = std::make_shared<Node>();
    // archive(node->name);
    archive(node->localTransform);
    archive(node->worldTransform);
    archive(node->isTransformDirty);
    bool hasMeshGroup;
    archive(hasMeshGroup);
    if (!hasMeshGroup) {
        node->meshGroup = nullptr;
    } else {
        // archive(node->meshGroup->name);
        auto meshGroup = std::make_shared<MeshGroup>();
        Uint32 meshCount;
        archive(meshCount);
        meshGroup->meshes.reserve(meshCount);
        for (Uint32 i = 0; i < meshCount; ++i) {
            meshGroup->meshes.push_back(deserializeMesh(archive, materials));
        }
        node->meshGroup = meshGroup;
    }
    Uint32 childCount;
    archive(childCount);
    node->children.reserve(childCount);
    for (Uint32 i = 0; i < childCount; ++i) {
        node->children.push_back(deserializeNode(archive, materials));
    }
    return node;
}

void AssetSerializer::serializeMaterial(cereal::BinaryOutputArchive& archive, const std::shared_ptr<Material>& material,
                                                   const std::unordered_map<std::shared_ptr<Image>, Uint32>& imageIDs) {
    if (!material) {
        archive(false);
        return;
    }
    archive(true);
    // archive(material->name);
    archive(static_cast<int>(material->alphaMode));
    archive(material->alphaCutoff);
    archive(material->doubleSided);
    archive(material->baseColorFactor);
    archive(material->normalScale);
    archive(material->metallicFactor);
    archive(material->roughnessFactor);
    archive(material->occlusionStrength);
    archive(material->emissiveFactor);

    auto serializeImageID = [&](const std::shared_ptr<Image>& image) {
        if (!image) {
            archive(static_cast<Uint32>(-1));
        } else {
            auto it = imageIDs.find(image);
            if (it != imageIDs.end()) {
                archive(it->second);
            } else {
                archive(static_cast<Uint32>(-1));
            }
        }
    };

    serializeImageID(material->albedoMap);
    serializeImageID(material->normalMap);
    serializeImageID(material->metallicRoughnessMap);
    serializeImageID(material->occlusionMap);
    serializeImageID(material->emissiveMap);
    // serializeImageID(material->displacementMap);
}

std::shared_ptr<Material> AssetSerializer::deserializeMaterial(cereal::BinaryInputArchive& archive,
                                                                          const std::unordered_map<Uint32, std::shared_ptr<Image>>& images) {
    bool isNotNull;
    archive(isNotNull);
    if (!isNotNull) {
        return nullptr;
    }
    auto material = std::make_shared<Material>();
    // archive(material->name);

    int alphaModeInt;
    archive(alphaModeInt);
    material->alphaMode = static_cast<AlphaMode>(alphaModeInt);

    archive(material->alphaCutoff);
    archive(material->doubleSided);
    archive(material->baseColorFactor);
    archive(material->normalScale);
    archive(material->metallicFactor);
    archive(material->roughnessFactor);
    archive(material->occlusionStrength);
    archive(material->emissiveFactor);

    auto deserializeImageID = [&]() -> std::shared_ptr<Image> {
        Uint32 imageID;
        archive(imageID);
        if (imageID == static_cast<Uint32>(-1)) {
            return nullptr;
        }
        auto it = images.find(imageID);
        if (it != images.end()) {
            return it->second;
        }
        return nullptr;
    };

    material->albedoMap = deserializeImageID();
    material->normalMap = deserializeImageID();
    material->metallicRoughnessMap = deserializeImageID();
    material->occlusionMap = deserializeImageID();
    material->emissiveMap = deserializeImageID();
    // material->displacementMap = deserializeImageID();

    return material;
}

void AssetSerializer::serializeImage(cereal::BinaryOutputArchive& archive, const std::shared_ptr<Image>& image) {
    if (!image) {
        archive(false);
        return;
    }

    archive(true);
    archive(image->uri);
    archive(image->width);
    archive(image->height);
    archive(image->channelCount);
    archive(image->byteArray);
}

std::shared_ptr<Image> AssetSerializer::deserializeImage(cereal::BinaryInputArchive& archive) {
    bool isNotNull;
    archive(isNotNull);
    if (!isNotNull) {
        return nullptr;
    }
    auto image = std::make_shared<Image>();

    archive(image->uri);
    archive(image->width);
    archive(image->height);
    archive(image->channelCount);
    archive(image->byteArray);

    return image;
}

void AssetSerializer::serializeMesh(cereal::BinaryOutputArchive& archive, const std::shared_ptr<Mesh>& mesh,
                                                   const std::unordered_map<std::shared_ptr<Material>, Uint32>& materialIDs) {
    if (!mesh) {
        archive(false);
        return;
    }
    archive(true);
    archive(mesh->bufferSize);
    archive(mesh->vertexCount);
    archive(mesh->indexCount);
    archive(mesh->hasPosition);
    archive(mesh->hasNormal);
    archive(mesh->hasTangent);
    archive(mesh->hasUV0);
    archive(mesh->hasUV1);
    archive(mesh->hasColor);
    archive(mesh->vertices);
    archive(mesh->indices);
    archive(static_cast<int>(mesh->primitiveMode));

    if (mesh->material) {
        auto it = materialIDs.find(mesh->material);
        if (it != materialIDs.end()) {
            archive(true);
            archive(it->second);
        } else {
            archive(false);
        }
    } else {
        archive(false);
    }
}

std::shared_ptr<Mesh> AssetSerializer::deserializeMesh(cereal::BinaryInputArchive& archive,
                                                                     const std::unordered_map<Uint32, std::shared_ptr<Material>>& materials) {
    bool isNotNull;
    archive(isNotNull);
    if (!isNotNull) {
        return nullptr;
    }
    auto mesh = std::make_shared<Mesh>();

    archive(mesh->bufferSize);
    archive(mesh->vertexCount);
    archive(mesh->indexCount);
    archive(mesh->hasPosition);
    archive(mesh->hasNormal);
    archive(mesh->hasTangent);
    archive(mesh->hasUV0);
    archive(mesh->hasUV1);
    archive(mesh->hasColor);
    archive(mesh->vertices);
    archive(mesh->indices);

    int primitiveModeInt;
    archive(primitiveModeInt);
    mesh->primitiveMode = static_cast<PrimitiveMode>(primitiveModeInt);

    bool hasMaterial;
    archive(hasMaterial);
    if (hasMaterial) {
        Uint32 materialID;
        archive(materialID);
        auto it = materials.find(materialID);
        if (it != materials.end()) {
            mesh->material = it->second;
        }
    } else {
        mesh->material = nullptr;
    }

    return mesh;
}

void AssetSerializer::serializeDirectionalLight(cereal::BinaryOutputArchive& archive, const DirectionalLight& light) {
    archive(light.direction);
    archive(light.color);
    archive(light.intensity);
}

DirectionalLight AssetSerializer::deserializeDirectionalLight(cereal::BinaryInputArchive& archive) {
    DirectionalLight light;
    archive(light.direction);
    archive(light.color);
    archive(light.intensity);
    return light;
}

void AssetSerializer::serializePointLight(cereal::BinaryOutputArchive& archive, const PointLight& light) {
    archive(light.position);
    archive(light.color);
    archive(light.intensity);
    archive(light.radius);
}

PointLight AssetSerializer::deserializePointLight(cereal::BinaryInputArchive& archive) {
    PointLight light;
    archive(light.position);
    archive(light.color);
    archive(light.intensity);
    archive(light.radius);
    return light;
}