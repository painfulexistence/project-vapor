#include "asset_manager.hpp"

#include "Vapor/file_system.hpp"
#include <SDL3/SDL_stdinc.h>
#include <filesystem>
#include <fmt/core.h>
#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <unordered_map>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

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

using namespace Vapor;

auto AssetManager::loadImage(const std::string& filename) -> std::shared_ptr<Image> {
    std::string fullPath = FileSystem::instance().resolvePathOrThrow(filename);
    int width, height, numChannels;
    if (!stbi_info(fullPath.c_str(), &width, &height, &numChannels)) {
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
    uint8_t* data = stbi_load(fullPath.c_str(), &width, &height, &numChannels, desiredChannels);
    if (data) {
        auto image = std::make_shared<Image>(Image{
            .uri = filename,
            .width = static_cast<Uint32>(width),
            .height = static_cast<Uint32>(height),
            .channelCount = static_cast<Uint32>(desiredChannels),
            .byteArray = std::vector<Uint8>(data, data + width * height * desiredChannels) });
        stbi_image_free(data);
        return image;
    } else {
        stbi_image_free(data);
        return nullptr;
    }
}

auto AssetManager::loadHDRI(const std::string& filename) -> std::shared_ptr<Vapor::HDRImage> {
    std::string fullPath = FileSystem::instance().resolvePathOrThrow(filename);
    int width, height, numChannels;
    // stbi_loadf decodes RGBE/HDR to linear float RGB(A)
    float* data = stbi_loadf(fullPath.c_str(), &width, &height, &numChannels, 4);
    if (!data) {
        throw std::runtime_error(fmt::format("Failed to load HDRI at {}: {}\n", filename, stbi_failure_reason()));
    }
    auto img = std::make_shared<Vapor::HDRImage>(Vapor::HDRImage{
        .uri = filename,
        .width = static_cast<Uint32>(width),
        .height = static_cast<Uint32>(height),
        .floatArray = std::vector<float>(data, data + width * height * 4),
    });
    stbi_image_free(data);
    return img;
}

// TODO: use Scene instead of Mesh
auto AssetManager::loadOBJ(const std::string& filename, const std::string& mtl_basedir) -> std::shared_ptr<Mesh> {
    std::vector<VertexData> vertices;
    std::vector<Uint32> indices;

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string err;

    if (!tinyobj::LoadObj(
            &attrib,
            &shapes,
            &materials,
            &err,
            FileSystem::instance().resolvePathOrThrow(filename).c_str(),
            mtl_basedir.empty() ? nullptr : FileSystem::instance().resolvePathOrThrow(mtl_basedir).c_str()
        )) {
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
            material->metallicMap = AssetManager::loadImage(mtl_basedir + mat.metallic_texname);
        }
        if (!mat.roughness_texname.empty()) {
            material->roughnessMap = AssetManager::loadImage(mtl_basedir + mat.roughness_texname);
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
    mesh->initialize(vertices, indices);
    mesh->material = meshMaterials[0];

    return mesh;
}

auto AssetManager::loadGLTF(const std::string& filename) -> std::shared_ptr<Scene> {
    auto resolved = FileSystem::instance().resolvePath(filename);
    if (!resolved) {
        fmt::print("GLTF not found in any search path: {}\n", filename);
        return nullptr;
    }
    std::filesystem::path filePath(*resolved);
    std::filesystem::path scenePath(filePath);
    if (std::filesystem::exists(scenePath.replace_extension(".vscene_optimized"))) {
        return AssetSerializer::deserializeScene(scenePath.string());
    }

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;
    bool result = filePath.extension() == ".glb" ? loader.LoadBinaryFromFile(&model, &err, &warn, filePath.string())
                                                 : loader.LoadASCIIFromFile(&model, &err, &warn, filePath.string());
    if (!warn.empty()) fmt::print("GLTF Warning: {}\n", warn);
    if (!err.empty()) fmt::print("GLTF Error: {}\n", err);
    if (!result || model.scenes.empty()) {
        fmt::print("Failed to parse GLTF\n");
        return nullptr;
    }

    auto scene = std::make_shared<Scene>();

    // Move tinygltf's decoded image buffers — no copy
    std::vector<std::shared_ptr<Image>> images;
    images.reserve(model.images.size());
    for (auto& img : model.images) {
        images.push_back(std::make_shared<Image>(Image{
            .uri = img.uri,
            .width = static_cast<Uint32>(img.width),
            .height = static_cast<Uint32>(img.height),
            .channelCount = static_cast<Uint32>(img.component),
            .byteArray = std::move(img.image),
        }));
    }

    std::vector<std::shared_ptr<Material>> materials;
    materials.reserve(model.materials.size());
    for (const auto& mat : model.materials) {
        auto material = std::make_shared<Material>();
        material->name = mat.name;
        if (mat.alphaMode == "BLEND")
            material->alphaMode = AlphaMode::BLEND;
        else if (mat.alphaMode == "MASK")
            material->alphaMode = AlphaMode::MASK;
        else
            material->alphaMode = AlphaMode::OPAQUE;
        material->alphaCutoff = mat.alphaCutoff;
        material->doubleSided = mat.doubleSided;
        material->baseColorFactor = glm::vec4(
            mat.pbrMetallicRoughness.baseColorFactor[0],
            mat.pbrMetallicRoughness.baseColorFactor[1],
            mat.pbrMetallicRoughness.baseColorFactor[2],
            mat.pbrMetallicRoughness.baseColorFactor[3]
        );
        material->metallicFactor = mat.pbrMetallicRoughness.metallicFactor;
        material->roughnessFactor = mat.pbrMetallicRoughness.roughnessFactor;
        material->emissiveFactor = glm::vec3(mat.emissiveFactor[0], mat.emissiveFactor[1], mat.emissiveFactor[2]);
        material->normalScale = mat.normalTexture.scale;
        material->occlusionStrength = mat.occlusionTexture.strength;
        const auto assignTex = [&](int idx, std::shared_ptr<Image>& slot) {
            if (idx >= 0 && model.textures[idx].source >= 0) slot = images[model.textures[idx].source];
        };
        assignTex(mat.pbrMetallicRoughness.baseColorTexture.index, material->albedoMap);
        if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
            int src = model.textures[mat.pbrMetallicRoughness.metallicRoughnessTexture.index].source;
            if (src >= 0) {
                material->metallicMap = images[src];
                material->roughnessMap = images[src];
            }
        }
        assignTex(mat.normalTexture.index, material->normalMap);
        assignTex(mat.occlusionTexture.index, material->occlusionMap);
        assignTex(mat.emissiveTexture.index, material->emissiveMap);
        materials.push_back(material);
    }

    // Stride-aware accessor helpers
    const auto readVec3 = [&](const tinygltf::Accessor& acc, size_t i) -> glm::vec3 {
        const auto& bv = model.bufferViews[acc.bufferView];
        const size_t stride = bv.byteStride ? bv.byteStride : sizeof(float) * 3;
        const float* f = reinterpret_cast<const float*>(
            model.buffers[bv.buffer].data.data() + bv.byteOffset + acc.byteOffset + i * stride
        );
        return { f[0], f[1], f[2] };
    };
    const auto readVec4 = [&](const tinygltf::Accessor& acc, size_t i) -> glm::vec4 {
        const auto& bv = model.bufferViews[acc.bufferView];
        const size_t stride = bv.byteStride ? bv.byteStride : sizeof(float) * 4;
        const float* f = reinterpret_cast<const float*>(
            model.buffers[bv.buffer].data.data() + bv.byteOffset + acc.byteOffset + i * stride
        );
        return { f[0], f[1], f[2], f[3] };
    };
    const auto readTexcoord = [&](const tinygltf::Accessor& acc, size_t i) -> glm::vec2 {
        const auto& bv = model.bufferViews[acc.bufferView];
        const uint8_t* base = model.buffers[bv.buffer].data.data() + bv.byteOffset + acc.byteOffset;
        switch (acc.componentType) {
        case TINYGLTF_COMPONENT_TYPE_FLOAT: {
            const size_t stride = bv.byteStride ? bv.byteStride : sizeof(float) * 2;
            const float* f = reinterpret_cast<const float*>(base + i * stride);
            return { f[0], f[1] };
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
            const Uint8* v = base + i * (bv.byteStride ? bv.byteStride : 2u);
            return { v[0] / 255.0f, v[1] / 255.0f };
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            const size_t stride = bv.byteStride ? bv.byteStride : sizeof(Uint16) * 2;
            const Uint16* v = reinterpret_cast<const Uint16*>(base + i * stride);
            return { v[0] / 65535.0f, v[1] / 65535.0f };
        }
        default:
            return { 0.0f, 0.0f };
        }
    };

    const auto getLocalMatrix = [](const tinygltf::Node& node) -> glm::mat4 {
        if (!node.matrix.empty()) {
            return glm::mat4(
                node.matrix[0],
                node.matrix[1],
                node.matrix[2],
                node.matrix[3],
                node.matrix[4],
                node.matrix[5],
                node.matrix[6],
                node.matrix[7],
                node.matrix[8],
                node.matrix[9],
                node.matrix[10],
                node.matrix[11],
                node.matrix[12],
                node.matrix[13],
                node.matrix[14],
                node.matrix[15]
            );
        }
        const auto t =
            node.translation.empty()
                ? glm::mat4(1.0f)
                : glm::translate(
                      glm::mat4(1.0f), glm::vec3(node.translation[0], node.translation[1], node.translation[2])
                  );
        const auto r =
            node.rotation.empty()
                ? glm::quat(1, 0, 0, 0)
                : glm::quat(
                      float(node.rotation[3]), float(node.rotation[0]), float(node.rotation[1]), float(node.rotation[2])
                  );
        const auto tr = t * glm::mat4_cast(r);
        return node.scale.empty() ? tr : glm::scale(tr, glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
    };

    // Mesh cache: GLTF mesh index → primitive handles already written into scene flat buffer.
    // Multiple scene nodes referencing the same GLTF mesh reuse vertex data without duplication.
    std::unordered_map<int, std::vector<std::shared_ptr<Mesh>>> meshCache;
    Uint32 vtxOffset = 0;
    Uint32 idxOffset = 0;

    const auto processMesh = [&](int meshIdx) -> const std::vector<std::shared_ptr<Mesh>>& {
        auto it = meshCache.find(meshIdx);
        if (it != meshCache.end()) return it->second;

        std::vector<std::shared_ptr<Mesh>> primitives;
        for (const auto& prim : model.meshes[meshIdx].primitives) {
            if (!prim.attributes.count("POSITION")) continue;

            const auto& posAcc = model.accessors[prim.attributes.at("POSITION")];
            const Uint32 vCount = static_cast<Uint32>(posAcc.count);

            const bool hasNormal = prim.attributes.count("NORMAL") > 0;
            const bool hasTangent = prim.attributes.count("TANGENT") > 0;
            const bool hasUV0 = prim.attributes.count("TEXCOORD_0") > 0;
            const bool hasUV1 = prim.attributes.count("TEXCOORD_1") > 0;
            const bool hasColor = prim.attributes.count("COLOR_0") > 0;

            // Append directly to scene flat buffer — no intermediate per-mesh copy
            scene->vertices.resize(vtxOffset + vCount);
            for (size_t i = 0; i < vCount; i++)
                scene->vertices[vtxOffset + i].position = readVec3(posAcc, i);
            if (hasNormal) {
                const auto& acc = model.accessors[prim.attributes.at("NORMAL")];
                for (size_t i = 0; i < vCount; i++)
                    scene->vertices[vtxOffset + i].normal = readVec3(acc, i);
            }
            if (hasTangent) {
                const auto& acc = model.accessors[prim.attributes.at("TANGENT")];
                for (size_t i = 0; i < vCount; i++)
                    scene->vertices[vtxOffset + i].tangent = readVec4(acc, i);
            }
            if (hasUV0) {
                const auto& acc = model.accessors[prim.attributes.at("TEXCOORD_0")];
                for (size_t i = 0; i < vCount; i++)
                    scene->vertices[vtxOffset + i].uv = readTexcoord(acc, i);
            }

            Uint32 iCount = 0;
            if (prim.indices >= 0) {
                const auto& idxAcc = model.accessors[prim.indices];
                const auto& idxBv = model.bufferViews[idxAcc.bufferView];
                const uint8_t* base = model.buffers[idxBv.buffer].data.data() + idxBv.byteOffset + idxAcc.byteOffset;
                iCount = static_cast<Uint32>(idxAcc.count);
                scene->indices.resize(idxOffset + iCount);
                switch (idxAcc.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    for (size_t i = 0; i < idxAcc.count; i++) {
                        Uint16 v;
                        std::memcpy(&v, base + i * sizeof(Uint16), sizeof(Uint16));
                        scene->indices[idxOffset + i] = v;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    for (size_t i = 0; i < idxAcc.count; i++) {
                        Uint32 v;
                        std::memcpy(&v, base + i * sizeof(Uint32), sizeof(Uint32));
                        scene->indices[idxOffset + i] = v;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    for (size_t i = 0; i < idxAcc.count; i++)
                        scene->indices[idxOffset + i] = base[i];
                    break;
                default:
                    fmt::print("Unsupported index type: {}\n", idxAcc.componentType);
                    break;
                }
            }

            auto mesh = std::make_shared<Mesh>();
            mesh->hasPosition = true;
            mesh->hasNormal = hasNormal;
            mesh->hasTangent = hasTangent;
            mesh->hasUV0 = hasUV0;
            mesh->hasUV1 = hasUV1;
            mesh->hasColor = hasColor;
            mesh->vertexOffset = vtxOffset;
            mesh->indexOffset = idxOffset;
            mesh->vertexCount = vCount;
            mesh->indexCount = iCount;
            mesh->isGeometryDirty = false;
            mesh->material = prim.material >= 0 ? materials[prim.material] : nullptr;
            if (posAcc.minValues.size() >= 3 && posAcc.maxValues.size() >= 3) {
                mesh->localAABBMin = glm::vec3(posAcc.minValues[0], posAcc.minValues[1], posAcc.minValues[2]);
                mesh->localAABBMax = glm::vec3(posAcc.maxValues[0], posAcc.maxValues[1], posAcc.maxValues[2]);
            }
            switch (prim.mode) {
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
                mesh->primitiveMode = PrimitiveMode::TRIANGLES;
                break;
            }

            vtxOffset += vCount;
            idxOffset += iCount;
            primitives.push_back(std::move(mesh));
        }

        meshCache.emplace(meshIdx, std::move(primitives));
        return meshCache.at(meshIdx);
    };

    const auto& srcScene = model.defaultScene >= 0 ? model.scenes[model.defaultScene] : model.scenes[0];
    scene->name = srcScene.name;

    // Single-pass traversal: build flat buffer and staged draw list together.
    // processMesh is called at most once per unique GLTF mesh index.
    std::function<void(int, const glm::mat4&)> processNode = [&](int nodeIdx, const glm::mat4& parentWorld) {
        const auto& srcNode = model.nodes[nodeIdx];
        const glm::mat4 world = parentWorld * getLocalMatrix(srcNode);
        if (srcNode.mesh >= 0) {
            for (const auto& mesh : processMesh(srcNode.mesh)) {
                scene->stagedMeshes.push_back(mesh);
                scene->stagedMeshTransforms.push_back(world);
            }
        }
        for (int childIdx : srcNode.children)
            processNode(childIdx, world);
    };

    for (int nodeIdx : srcScene.nodes)
        processNode(nodeIdx, glm::identity<glm::mat4>());

    // materials/images are referenced (by index) by processMesh above — move
    // into scene only after the traversal is done with them.
    scene->images = std::move(images);
    scene->materials = std::move(materials);

    fmt::print(
        "Optimized scene: {} vertices, {} indices, {} draw calls\n",
        scene->vertices.size(),
        scene->indices.size(),
        scene->stagedMeshes.size()
    );

    AssetSerializer::serializeScene(scene, scenePath.string());
    return scene;
}