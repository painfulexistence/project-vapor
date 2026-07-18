#include "asset_serializer.hpp"
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <fmt/core.h>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

using namespace Vapor;

void AssetSerializer::serializeScene(const std::shared_ptr<RenderScene>& scene, const std::string& path) {
    auto start = SDL_GetTicks();

    {
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error(fmt::format("Failed to open file for writing: {}", path));
        }

        cereal::BinaryOutputArchive archive(file);
        archive(SCENE_FORMAT_VERSION);
        archive(scene->name);
        archive(scene->vertices);
        archive(scene->indices);

        std::unordered_map<std::shared_ptr<Image>, Uint32> imageIDs;
        std::vector<std::shared_ptr<Image>> uniqueImages;
        for (const auto& img : scene->images) {
            if (img && imageIDs.find(img) == imageIDs.end()) {
                imageIDs[img] = static_cast<Uint32>(uniqueImages.size());
                uniqueImages.push_back(img);
            }
        }

        archive(static_cast<Uint32>(uniqueImages.size()));
        for (Uint32 i = 0; i < uniqueImages.size(); ++i) {
            archive(i);
            serializeImage(archive, uniqueImages[i]);
        }

        std::unordered_map<std::shared_ptr<Material>, Uint32> materialIDs;
        std::vector<std::shared_ptr<Material>> uniqueMaterials;
        for (const auto& mat : scene->materials) {
            if (mat && materialIDs.find(mat) == materialIDs.end()) {
                materialIDs[mat] = static_cast<Uint32>(uniqueMaterials.size());
                uniqueMaterials.push_back(mat);
            }
        }

        archive(static_cast<Uint32>(uniqueMaterials.size()));
        for (Uint32 i = 0; i < uniqueMaterials.size(); ++i) {
            archive(i);
            serializeMaterial(archive, uniqueMaterials[i], imageIDs);
        }

        archive(static_cast<Uint32>(scene->directionalLights.size()));
        for (const auto& light : scene->directionalLights) {
            serializeDirectionalLight(archive, light);
        }

        archive(static_cast<Uint32>(scene->pointLights.size()));
        for (const auto& light : scene->pointLights) {
            serializePointLight(archive, light);
        }

        archive(static_cast<Uint32>(scene->stagedMeshes.size()));
        for (size_t i = 0; i < scene->stagedMeshes.size(); ++i) {
            serializeMesh(archive, scene->stagedMeshes[i], materialIDs);
            glm::mat4 t = i < scene->stagedMeshTransforms.size()
                ? scene->stagedMeshTransforms[i]
                : glm::identity<glm::mat4>();
            archive(t);
        }

    }// Ensure archive is flushed
    fmt::print("Scene serialized to: {} in {} ms\n", path, SDL_GetTicks() - start);
}

auto AssetSerializer::deserializeScene(const std::string& path) -> std::shared_ptr<RenderScene> {
    auto start = SDL_GetTicks();

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error(fmt::format("Failed to open file for reading: {}", path));
    }

    cereal::BinaryInputArchive archive(file);
    uint32_t version = 0;
    try {
        archive(version);
    } catch (...) {
        throw std::runtime_error(fmt::format(
            "Failed to read scene format version from: {}. Delete the cache file and re-run.", path
        ));
    }
    if (version != SCENE_FORMAT_VERSION) {
        throw std::runtime_error(fmt::format(
            "Scene cache version mismatch in {}: expected {}, got {}. Delete the cache file and re-run.",
            path, SCENE_FORMAT_VERSION, version
        ));
    }
    auto scene = std::make_shared<RenderScene>();
    archive(scene->name);
    archive(scene->vertices);
    archive(scene->indices);

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

    Uint32 directionalLightCount;
    archive(directionalLightCount);
    scene->directionalLights.reserve(directionalLightCount);
    for (Uint32 i = 0; i < directionalLightCount; ++i) {
        Vapor::DirectionalLight light = deserializeDirectionalLight(archive);
        scene->directionalLights.push_back(light);
    }

    Uint32 pointLightCount;
    archive(pointLightCount);
    scene->pointLights.reserve(pointLightCount);
    for (Uint32 i = 0; i < pointLightCount; ++i) {
        Vapor::PointLight light = deserializePointLight(archive);
        scene->pointLights.push_back(light);
    }

    Uint32 meshCount;
    archive(meshCount);
    scene->stagedMeshes.reserve(meshCount);
    scene->stagedMeshTransforms.reserve(meshCount);
    for (Uint32 i = 0; i < meshCount; ++i) {
        scene->stagedMeshes.push_back(deserializeMesh(archive, materials));
        glm::mat4 t;
        archive(t);
        scene->stagedMeshTransforms.push_back(t);
    }

    fmt::print("Scene deserialized from: {} in {} ms\n", path, SDL_GetTicks() - start);
    return scene;
}

void AssetSerializer::serializeMaterial(
    cereal::BinaryOutputArchive& archive,
    const std::shared_ptr<Material>& material,
    const std::unordered_map<std::shared_ptr<Image>, Uint32>& imageIDs
) {
    if (!material) {
        archive(false);
        return;
    }
    archive(true);
    archive(material->name);
    archive(static_cast<int>(material->alphaMode));
    archive(material->alphaCutoff);
    archive(material->doubleSided);
    archive(material->baseColorFactor);
    archive(material->normalScale);
    archive(material->metallicFactor);
    archive(material->roughnessFactor);
    archive(material->occlusionStrength);
    archive(material->emissiveFactor);
    archive(material->emissiveStrength);
    archive(material->subsurface);
    archive(material->specular);
    archive(material->specularTint);
    archive(material->anisotropic);
    archive(material->sheen);
    archive(material->sheenTint);
    archive(material->clearcoat);
    archive(material->clearcoatGloss);

    auto serializeImageID = [&](const std::shared_ptr<Image>& image) -> void {
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
    serializeImageID(material->metallicMap);
    serializeImageID(material->roughnessMap);
    serializeImageID(material->occlusionMap);
    serializeImageID(material->emissiveMap);
    // serializeImageID(material->displacementMap);
}

auto AssetSerializer::deserializeMaterial(
    cereal::BinaryInputArchive& archive, const std::unordered_map<Uint32, std::shared_ptr<Image>>& images
) -> std::shared_ptr<Material> {
    bool isNotNull;
    archive(isNotNull);
    if (!isNotNull) {
        return nullptr;
    }
    auto material = std::make_shared<Material>();
    archive(material->name);

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
    archive(material->emissiveStrength);
    archive(material->subsurface);
    archive(material->specular);
    archive(material->specularTint);
    archive(material->anisotropic);
    archive(material->sheen);
    archive(material->sheenTint);
    archive(material->clearcoat);
    archive(material->clearcoatGloss);

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
    material->metallicMap = deserializeImageID();
    material->roughnessMap = deserializeImageID();
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

auto AssetSerializer::deserializeImage(cereal::BinaryInputArchive& archive) -> std::shared_ptr<Image> {
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

void AssetSerializer::serializeMesh(
    cereal::BinaryOutputArchive& archive,
    const std::shared_ptr<Mesh>& mesh,
    const std::unordered_map<std::shared_ptr<Material>, Uint32>& materialIDs
) {
    if (!mesh) {
        archive(false);
        return;
    }
    archive(true);
    archive(mesh->hasPosition);
    archive(mesh->hasNormal);
    archive(mesh->hasTangent);
    archive(mesh->hasUV0);
    archive(mesh->hasUV1);
    archive(mesh->hasColor);
    archive(mesh->vertices);
    archive(mesh->indices);
    archive(static_cast<int>(mesh->primitiveMode));

    archive(mesh->vertexOffset);
    archive(mesh->indexOffset);
    archive(mesh->vertexCount);
    archive(mesh->indexCount);
    archive(mesh->localAABBMin);
    archive(mesh->localAABBMax);

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

auto AssetSerializer::deserializeMesh(
    cereal::BinaryInputArchive& archive, const std::unordered_map<Uint32, std::shared_ptr<Material>>& materials
) -> std::shared_ptr<Mesh> {
    bool isNotNull;
    archive(isNotNull);
    if (!isNotNull) {
        return nullptr;
    }
    auto mesh = std::make_shared<Mesh>();

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

    archive(mesh->vertexOffset);
    archive(mesh->indexOffset);
    archive(mesh->vertexCount);
    archive(mesh->indexCount);
    archive(mesh->localAABBMin);
    archive(mesh->localAABBMax);
    mesh->isGeometryDirty = false;// prevent AABB updating

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

void AssetSerializer::serializeDirectionalLight(cereal::BinaryOutputArchive& archive, const Vapor::DirectionalLight& light) {
    archive(light.direction);
    archive(light.color);
    archive(light.intensity);
}

auto AssetSerializer::deserializeDirectionalLight(cereal::BinaryInputArchive& archive) -> Vapor::DirectionalLight {
    Vapor::DirectionalLight light;
    archive(light.direction);
    archive(light.color);
    archive(light.intensity);
    return light;
}

void AssetSerializer::serializePointLight(cereal::BinaryOutputArchive& archive, const Vapor::PointLight& light) {
    archive(light.position);
    archive(light.color);
    archive(light.intensity);
    archive(light.radius);
}

auto AssetSerializer::deserializePointLight(cereal::BinaryInputArchive& archive) -> Vapor::PointLight {
    Vapor::PointLight light;
    archive(light.position);
    archive(light.color);
    archive(light.intensity);
    archive(light.radius);
    return light;
}
// ── SceneBlueprint body (used by the .vscene scene cook) ────────────────────

void AssetSerializer::serializeBlueprint(cereal::BinaryOutputArchive& archive, const Vapor::SceneBlueprint& blueprint) {
    archive(BLUEPRINT_FORMAT_VERSION);
    archive(blueprint.name);

    // Unique images: the blueprint list plus any image a material references
    // that the importer didn't register (defensive; importers do register all).
    std::unordered_map<std::shared_ptr<Image>, Uint32> imageIDs;
    std::vector<std::shared_ptr<Image>> uniqueImages;
    auto addImage = [&](const std::shared_ptr<Image>& img) {
        if (img && imageIDs.find(img) == imageIDs.end()) {
            imageIDs[img] = static_cast<Uint32>(uniqueImages.size());
            uniqueImages.push_back(img);
        }
    };
    for (const auto& img : blueprint.images)
        addImage(img);
    for (const auto& mat : blueprint.materials) {
        if (!mat) continue;
        addImage(mat->albedoMap);
        addImage(mat->normalMap);
        addImage(mat->metallicMap);
        addImage(mat->roughnessMap);
        addImage(mat->occlusionMap);
        addImage(mat->emissiveMap);
        addImage(mat->displacementMap);
    }
    archive(static_cast<Uint32>(uniqueImages.size()));
    for (const auto& img : uniqueImages)
        serializeImage(archive, img);

    std::unordered_map<std::shared_ptr<Material>, Uint32> materialIDs;
    std::vector<std::shared_ptr<Material>> uniqueMaterials;
    auto addMaterial = [&](const std::shared_ptr<Material>& mat) {
        if (mat && materialIDs.find(mat) == materialIDs.end()) {
            materialIDs[mat] = static_cast<Uint32>(uniqueMaterials.size());
            uniqueMaterials.push_back(mat);
        }
    };
    for (const auto& mat : blueprint.materials)
        addMaterial(mat);
    for (const auto& mesh : blueprint.meshes)
        if (mesh) addMaterial(mesh->material);
    archive(static_cast<Uint32>(uniqueMaterials.size()));
    for (const auto& mat : uniqueMaterials)
        serializeMaterial(archive, mat, imageIDs);

    archive(static_cast<Uint32>(blueprint.meshes.size()));
    for (const auto& mesh : blueprint.meshes)
        serializeMesh(archive, mesh, materialIDs);

    archive(static_cast<Uint32>(blueprint.lights.size()));
    for (const auto& light : blueprint.lights) {
        archive(static_cast<int>(light.type));
        archive(light.color);
        archive(light.intensity);
        archive(light.range);
        archive(light.innerConeAngle);
        archive(light.outerConeAngle);
    }

    archive(static_cast<Uint32>(blueprint.entities.size()));
    for (const auto& e : blueprint.entities) {
        archive(e.name);
        archive(e.position);
        archive(e.rotation);
        archive(e.scale);
        archive(e.parent);
        archive(e.meshes);
        archive(e.lights);
        archive(e.source);
        archive(e.prefab);
        archive(e.componentsJson);
    }

    archive(blueprint.sources);
}

auto AssetSerializer::deserializeBlueprint(cereal::BinaryInputArchive& archive) -> Vapor::SceneBlueprint {
    Vapor::SceneBlueprint blueprint;
    uint32_t version = 0;
    archive(version);
    if (version != BLUEPRINT_FORMAT_VERSION) return blueprint;// ok == false
    archive(blueprint.name);

    Uint32 imageCount = 0;
    archive(imageCount);
    std::unordered_map<Uint32, std::shared_ptr<Image>> images;
    blueprint.images.reserve(imageCount);
    for (Uint32 i = 0; i < imageCount; ++i) {
        auto img = deserializeImage(archive);
        images[i] = img;
        blueprint.images.push_back(std::move(img));
    }

    Uint32 materialCount = 0;
    archive(materialCount);
    std::unordered_map<Uint32, std::shared_ptr<Material>> materials;
    blueprint.materials.reserve(materialCount);
    for (Uint32 i = 0; i < materialCount; ++i) {
        auto mat = deserializeMaterial(archive, images);
        materials[i] = mat;
        blueprint.materials.push_back(std::move(mat));
    }

    Uint32 meshCount = 0;
    archive(meshCount);
    blueprint.meshes.reserve(meshCount);
    for (Uint32 i = 0; i < meshCount; ++i)
        blueprint.meshes.push_back(deserializeMesh(archive, materials));

    Uint32 lightCount = 0;
    archive(lightCount);
    blueprint.lights.reserve(lightCount);
    for (Uint32 i = 0; i < lightCount; ++i) {
        Vapor::LightBlueprint light;
        int type = 0;
        archive(type);
        light.type = static_cast<Vapor::LightBlueprint::Type>(type);
        archive(light.color);
        archive(light.intensity);
        archive(light.range);
        archive(light.innerConeAngle);
        archive(light.outerConeAngle);
        blueprint.lights.push_back(light);
    }

    Uint32 entityCount = 0;
    archive(entityCount);
    blueprint.entities.reserve(entityCount);
    for (Uint32 i = 0; i < entityCount; ++i) {
        Vapor::EntityBlueprint e;
        archive(e.name);
        archive(e.position);
        archive(e.rotation);
        archive(e.scale);
        archive(e.parent);
        archive(e.meshes);
        archive(e.lights);
        archive(e.source);
        archive(e.prefab);
        archive(e.componentsJson);
        blueprint.entities.push_back(std::move(e));
    }

    archive(blueprint.sources);
    blueprint.ok = true;
    return blueprint;
}
