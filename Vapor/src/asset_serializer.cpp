#include "asset_serializer.hpp"
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>
#include <fmt/core.h>
#include <fstream>
#include <unordered_map>

using namespace Vapor;

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

    // Baked meshlet + cluster-LOD data (v4). Empty vectors when not built.
    archive(mesh->meshletData.meshlets);
    archive(mesh->meshletData.meshletVertices);
    archive(mesh->meshletData.meshletTriangles);
    archive(mesh->meshletData.bounds);
    archive(mesh->meshletData.lodLevelCount);

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

    // Baked meshlet + cluster-LOD data (v4), written after localAABBMax above.
    archive(mesh->meshletData.meshlets);
    archive(mesh->meshletData.meshletVertices);
    archive(mesh->meshletData.meshletTriangles);
    archive(mesh->meshletData.bounds);
    archive(mesh->meshletData.lodLevelCount);

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
        archive(static_cast<uint8_t>(e.primitive.shape));
        archive(e.primitive.size);
        archive(e.primitive.height);
        archive(e.primitive.material);

        // v5: per-node animation clips
        archive(static_cast<Uint32>(e.clips.size()));
        for (const auto& clip : e.clips) {
            archive(clip.name);
            archive(static_cast<Uint32>(clip.tracks.size()));
            for (const auto& track : clip.tracks) {
                archive(static_cast<int>(track.property));
                archive(track.customId);
                archive(static_cast<Uint32>(track.keys.size()));
                for (const auto& key : track.keys) {
                    archive(key.time);
                    archive(key.value);
                    archive(static_cast<int>(key.easing));
                }
            }
        }
    }

    // v5: skeletons + skeletal clips
    archive(static_cast<Uint32>(blueprint.skeletons.size()));
    for (const auto& skel : blueprint.skeletons) {
        archive(skel.name);
        archive(static_cast<Uint32>(skel.joints.size()));
        for (const auto& j : skel.joints) {
            archive(j.name);
            archive(j.parent);
            archive(j.bindTranslation);
            archive(j.bindRotation);
            archive(j.bindScale);
            archive(j.inverseBind);
        }
    }
    archive(static_cast<Uint32>(blueprint.skeletonClips.size()));
    for (const auto& sc : blueprint.skeletonClips) {
        archive(sc.skeleton);
        archive(sc.clip.name);
        archive(static_cast<Uint32>(sc.clip.channels.size()));
        for (const auto& ch : sc.clip.channels) {
            archive(ch.joint);
            archive(static_cast<Uint32>(ch.translation.size()));
            for (const auto& k : ch.translation) {
                archive(k.time);
                archive(k.value);
            }
            archive(static_cast<Uint32>(ch.rotation.size()));
            for (const auto& k : ch.rotation) {
                archive(k.time);
                archive(k.value);
            }
            archive(static_cast<Uint32>(ch.scale.size()));
            for (const auto& k : ch.scale) {
                archive(k.time);
                archive(k.value);
            }
        }
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
        uint8_t shape = 0;
        archive(shape);
        e.primitive.shape = static_cast<Vapor::PrimitiveBlueprint::Shape>(shape);
        archive(e.primitive.size);
        archive(e.primitive.height);
        archive(e.primitive.material);

        // v5: per-node animation clips
        Uint32 clipCount = 0;
        archive(clipCount);
        e.clips.reserve(clipCount);
        for (Uint32 c = 0; c < clipCount; ++c) {
            Vapor::NodeClipBlueprint clip;
            archive(clip.name);
            Uint32 trackCount = 0;
            archive(trackCount);
            clip.tracks.reserve(trackCount);
            for (Uint32 t = 0; t < trackCount; ++t) {
                Vapor::ActionTrack track;
                int prop = 0;
                archive(prop);
                track.property = static_cast<Vapor::ActionProperty>(prop);
                archive(track.customId);
                Uint32 keyCount = 0;
                archive(keyCount);
                track.keys.reserve(keyCount);
                for (Uint32 k = 0; k < keyCount; ++k) {
                    Vapor::ActionKey key;
                    archive(key.time);
                    archive(key.value);
                    int easing = 0;
                    archive(easing);
                    key.easing = static_cast<Vapor::EasingType>(easing);
                    track.keys.push_back(key);
                }
                clip.tracks.push_back(std::move(track));
            }
            e.clips.push_back(std::move(clip));
        }

        blueprint.entities.push_back(std::move(e));
    }

    // v5: skeletons + skeletal clips
    Uint32 skeletonCount = 0;
    archive(skeletonCount);
    blueprint.skeletons.reserve(skeletonCount);
    for (Uint32 i = 0; i < skeletonCount; ++i) {
        Vapor::Skeleton skel;
        archive(skel.name);
        Uint32 jointCount = 0;
        archive(jointCount);
        skel.joints.reserve(jointCount);
        for (Uint32 j = 0; j < jointCount; ++j) {
            Vapor::Joint joint;
            archive(joint.name);
            archive(joint.parent);
            archive(joint.bindTranslation);
            archive(joint.bindRotation);
            archive(joint.bindScale);
            archive(joint.inverseBind);
            skel.joints.push_back(std::move(joint));
        }
        blueprint.skeletons.push_back(std::move(skel));
    }
    Uint32 skeletonClipCount = 0;
    archive(skeletonClipCount);
    blueprint.skeletonClips.reserve(skeletonClipCount);
    for (Uint32 i = 0; i < skeletonClipCount; ++i) {
        Vapor::SkeletonClipBlueprint sc;
        archive(sc.skeleton);
        archive(sc.clip.name);
        Uint32 channelCount = 0;
        archive(channelCount);
        sc.clip.channels.reserve(channelCount);
        for (Uint32 c = 0; c < channelCount; ++c) {
            Vapor::JointChannel ch;
            archive(ch.joint);
            Uint32 n = 0;
            archive(n);
            ch.translation.resize(n);
            for (auto& k : ch.translation) {
                archive(k.time);
                archive(k.value);
            }
            archive(n);
            ch.rotation.resize(n);
            for (auto& k : ch.rotation) {
                archive(k.time);
                archive(k.value);
            }
            archive(n);
            ch.scale.resize(n);
            for (auto& k : ch.scale) {
                archive(k.time);
                archive(k.value);
            }
            sc.clip.channels.push_back(std::move(ch));
        }
        sc.clip.recompute();
        blueprint.skeletonClips.push_back(std::move(sc));
    }

    archive(blueprint.sources);
    blueprint.ok = true;
    return blueprint;
}
