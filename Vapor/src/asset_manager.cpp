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

#include "graphics.hpp"
#include <cstring>
#include <functional>

using namespace Vapor;

// Asset/IO failures return nullptr (with a log) rather than throwing: bad or
// missing content must not take the engine down, and these also run on
// ResourceManager worker threads where an escaped exception is fatal.
auto AssetManager::loadImage(const std::string& filename) -> std::shared_ptr<Image> {
    auto fullPath = FileSystem::instance().resolvePath(filename);
    if (!fullPath) {
        fmt::print(stderr, "loadImage '{}': not found in any search path\n", filename);
        return nullptr;
    }
    int width, height, numChannels;
    if (!stbi_info(fullPath->c_str(), &width, &height, &numChannels)) {
        fmt::print(stderr, "loadImage '{}': {}\n", filename, stbi_failure_reason());
        return nullptr;
    }
    int desiredChannels = 0;
    switch (numChannels) {
    case 1:
        desiredChannels = 1;
        break;
    case 3:
    case 4:
        desiredChannels = 4;
        break;
    default:
        fmt::print(stderr, "loadImage '{}': unsupported channel count {}\n", filename, numChannels);
        return nullptr;
    }
    uint8_t* data = stbi_load(fullPath->c_str(), &width, &height, &numChannels, desiredChannels);
    if (!data) {
        fmt::print(stderr, "loadImage '{}': {}\n", filename, stbi_failure_reason());
        return nullptr;
    }
    auto image = std::make_shared<Image>(Image{
        .uri = filename,
        .width = static_cast<Uint32>(width),
        .height = static_cast<Uint32>(height),
        .channelCount = static_cast<Uint32>(desiredChannels),
        .byteArray = std::vector<Uint8>(data, data + width * height * desiredChannels) });
    stbi_image_free(data);
    return image;
}

auto AssetManager::loadHDRI(const std::string& filename) -> std::shared_ptr<Vapor::HDRImage> {
    auto fullPath = FileSystem::instance().resolvePath(filename);
    if (!fullPath) {
        fmt::print(stderr, "loadHDRI '{}': not found in any search path\n", filename);
        return nullptr;
    }
    int width, height, numChannels;
    // stbi_loadf decodes RGBE/HDR to linear float RGB(A)
    float* data = stbi_loadf(fullPath->c_str(), &width, &height, &numChannels, 4);
    if (!data) {
        fmt::print(stderr, "loadHDRI '{}': {}\n", filename, stbi_failure_reason());
        return nullptr;
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

    auto objPath = FileSystem::instance().resolvePath(filename);
    if (!objPath) {
        fmt::print(stderr, "loadOBJ '{}': not found in any search path\n", filename);
        return nullptr;
    }
    std::optional<std::string> mtlPath;
    if (!mtl_basedir.empty()) {
        mtlPath = FileSystem::instance().resolvePath(mtl_basedir);
        if (!mtlPath) {
            fmt::print(stderr, "loadOBJ '{}': mtl basedir '{}' not found\n", filename, mtl_basedir);
            return nullptr;
        }
    }
    if (!tinyobj::LoadObj(
            &attrib, &shapes, &materials, &err, objPath->c_str(), mtlPath ? mtlPath->c_str() : nullptr
        )) {
        fmt::print(stderr, "loadOBJ '{}': {}\n", filename, err);
        return nullptr;
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


auto AssetManager::loadGLTF(const std::string& filename) -> Vapor::SceneBlueprint {
    Vapor::SceneBlueprint bp;
    auto resolved = FileSystem::instance().resolvePath(filename);
    if (!resolved) {
        fmt::print("GLTF not found in any search path: {}\n", filename);
        return bp;
    }
    std::filesystem::path filePath(*resolved);

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;
    bool result = filePath.extension() == ".glb" ? loader.LoadBinaryFromFile(&model, &err, &warn, filePath.string())
                                                 : loader.LoadASCIIFromFile(&model, &err, &warn, filePath.string());
    if (!warn.empty()) fmt::print("GLTF Warning: {}\n", warn);
    if (!err.empty()) fmt::print("GLTF Error: {}\n", err);
    if (!result || model.scenes.empty()) {
        fmt::print("Failed to parse GLTF\n");
        return bp;
    }

    // Move tinygltf's decoded image buffers — no copy
    bp.images.reserve(model.images.size());
    for (auto& img : model.images) {
        bp.images.push_back(std::make_shared<Image>(Image{
            .uri = img.uri,
            .width = static_cast<Uint32>(img.width),
            .height = static_cast<Uint32>(img.height),
            .channelCount = static_cast<Uint32>(img.component),
            .byteArray = std::move(img.image),
        }));
    }

    bp.materials.reserve(model.materials.size());
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
            if (idx >= 0 && model.textures[idx].source >= 0) slot = bp.images[model.textures[idx].source];
        };
        assignTex(mat.pbrMetallicRoughness.baseColorTexture.index, material->albedoMap);
        if (mat.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0) {
            int src = model.textures[mat.pbrMetallicRoughness.metallicRoughnessTexture.index].source;
            if (src >= 0) {
                material->metallicMap = bp.images[src];
                material->roughnessMap = bp.images[src];
            }
        }
        assignTex(mat.normalTexture.index, material->normalMap);
        assignTex(mat.occlusionTexture.index, material->occlusionMap);
        assignTex(mat.emissiveTexture.index, material->emissiveMap);

        // --- glTF material extensions (factor-based) ------------------------
        // These map straight onto Disney fields the shaders already consume —
        // the ingestion gap was bigger than the rendering gap. Texture-based
        // extension inputs (clearcoat/sheen textures, KHR_texture_transform)
        // are follow-ups; transform in particular must reconcile with the
        // triplanar prototype-UV path first.
        {
            auto ext = mat.extensions.find("KHR_materials_emissive_strength");
            if (ext != mat.extensions.end() && ext->second.Has("emissiveStrength")) {
                material->emissiveStrength = static_cast<float>(ext->second.Get("emissiveStrength").GetNumberAsDouble());
            }

            ext = mat.extensions.find("KHR_materials_clearcoat");
            if (ext != mat.extensions.end()) {
                if (ext->second.Has("clearcoatFactor")) {
                    material->clearcoat = static_cast<float>(ext->second.Get("clearcoatFactor").GetNumberAsDouble());
                }
                // Disney's clearcoatGloss is the inverse of glTF's roughness.
                if (ext->second.Has("clearcoatRoughnessFactor")) {
                    material->clearcoatGloss =
                        1.0f - static_cast<float>(ext->second.Get("clearcoatRoughnessFactor").GetNumberAsDouble());
                }
            }

            ext = mat.extensions.find("KHR_materials_sheen");
            if (ext != mat.extensions.end() && ext->second.Has("sheenColorFactor")) {
                // The Disney sheen model here is scalar: take the strongest
                // channel as the strength and keep sheenTint at its default
                // (tinted toward the base color) — documented approximation.
                const auto& v = ext->second.Get("sheenColorFactor");
                float mx = 0.0f;
                for (int c = 0; c < 3 && c < static_cast<int>(v.ArrayLen()); ++c) {
                    mx = std::max(mx, static_cast<float>(v.Get(c).GetNumberAsDouble()));
                }
                material->sheen = mx;
            }

            ext = mat.extensions.find("KHR_materials_specular");
            if (ext != mat.extensions.end() && ext->second.Has("specularFactor")) {
                material->specular = static_cast<float>(ext->second.Get("specularFactor").GetNumberAsDouble());
            }
        }

        bp.materials.push_back(material);
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

    // KHR_lights_punctual: tinygltf parses the extension into model.lights and
    // Node::light. Angles stay in radians (the blueprint convention).
    bp.lights.reserve(model.lights.size());
    for (const auto& l : model.lights) {
        Vapor::LightBlueprint light;
        if (l.type == "directional")
            light.type = Vapor::LightBlueprint::Type::Directional;
        else if (l.type == "spot")
            light.type = Vapor::LightBlueprint::Type::Spot;
        else
            light.type = Vapor::LightBlueprint::Type::Point;
        if (l.color.size() >= 3) light.color = glm::vec3(l.color[0], l.color[1], l.color[2]);
        light.intensity = static_cast<float>(l.intensity);
        light.range = static_cast<float>(l.range);
        light.innerConeAngle = static_cast<float>(l.spot.innerConeAngle);
        light.outerConeAngle = static_cast<float>(l.spot.outerConeAngle);
        bp.lights.push_back(light);
    }

    // Mesh cache: GLTF mesh index -> blueprint mesh indices. Multiple nodes
    // referencing the same GLTF mesh share the decoded primitives, so repeated
    // use stays one geometry registration (GPU instancing at draw time).
    std::unordered_map<int, std::vector<int>> meshCache;
    const auto processMesh = [&](int meshIdx) -> const std::vector<int>& {
        auto it = meshCache.find(meshIdx);
        if (it != meshCache.end()) return it->second;

        std::vector<int> primitives;
        for (const auto& prim : model.meshes[meshIdx].primitives) {
            if (!prim.attributes.count("POSITION")) continue;

            const auto& posAcc = model.accessors[prim.attributes.at("POSITION")];
            const Uint32 vCount = static_cast<Uint32>(posAcc.count);

            const bool hasNormal = prim.attributes.count("NORMAL") > 0;
            const bool hasTangent = prim.attributes.count("TANGENT") > 0;
            const bool hasUV0 = prim.attributes.count("TEXCOORD_0") > 0;
            const bool hasUV1 = prim.attributes.count("TEXCOORD_1") > 0;
            const bool hasColor = prim.attributes.count("COLOR_0") > 0;

            auto mesh = std::make_shared<Mesh>();
            mesh->vertices.resize(vCount);
            for (size_t i = 0; i < vCount; i++)
                mesh->vertices[i].position = readVec3(posAcc, i);
            if (hasNormal) {
                const auto& acc = model.accessors[prim.attributes.at("NORMAL")];
                for (size_t i = 0; i < vCount; i++)
                    mesh->vertices[i].normal = readVec3(acc, i);
            }
            if (hasTangent) {
                const auto& acc = model.accessors[prim.attributes.at("TANGENT")];
                for (size_t i = 0; i < vCount; i++)
                    mesh->vertices[i].tangent = readVec4(acc, i);
            }
            if (hasUV0) {
                const auto& acc = model.accessors[prim.attributes.at("TEXCOORD_0")];
                for (size_t i = 0; i < vCount; i++)
                    mesh->vertices[i].uv = readTexcoord(acc, i);
            }

            if (prim.indices >= 0) {
                const auto& idxAcc = model.accessors[prim.indices];
                const auto& idxBv = model.bufferViews[idxAcc.bufferView];
                const uint8_t* base = model.buffers[idxBv.buffer].data.data() + idxBv.byteOffset + idxAcc.byteOffset;
                mesh->indices.resize(idxAcc.count);
                switch (idxAcc.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    for (size_t i = 0; i < idxAcc.count; i++) {
                        Uint16 v;
                        std::memcpy(&v, base + i * sizeof(Uint16), sizeof(Uint16));
                        mesh->indices[i] = v;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    for (size_t i = 0; i < idxAcc.count; i++) {
                        Uint32 v;
                        std::memcpy(&v, base + i * sizeof(Uint32), sizeof(Uint32));
                        mesh->indices[i] = v;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    for (size_t i = 0; i < idxAcc.count; i++)
                        mesh->indices[i] = base[i];
                    break;
                default:
                    fmt::print("Unsupported index type: {}\n", idxAcc.componentType);
                    break;
                }
            } else {
                // Non-indexed primitive: synthesize a sequential index list so
                // Renderer::stage() always has indices to register.
                mesh->indices.resize(vCount);
                for (Uint32 i = 0; i < vCount; i++)
                    mesh->indices[i] = i;
            }

            mesh->hasPosition = true;
            mesh->hasNormal = hasNormal;
            mesh->hasTangent = hasTangent;
            mesh->hasUV0 = hasUV0;
            mesh->hasUV1 = hasUV1;
            mesh->hasColor = hasColor;
            mesh->vertexCount = vCount;
            mesh->indexCount = static_cast<Uint32>(mesh->indices.size());
            mesh->isGeometryDirty = false;
            mesh->material = prim.material >= 0 ? bp.materials[prim.material] : nullptr;
            if (posAcc.minValues.size() >= 3 && posAcc.maxValues.size() >= 3) {
                mesh->localAABBMin = glm::vec3(posAcc.minValues[0], posAcc.minValues[1], posAcc.minValues[2]);
                mesh->localAABBMax = glm::vec3(posAcc.maxValues[0], posAcc.maxValues[1], posAcc.maxValues[2]);
            } else {
                mesh->calculateLocalAABB();
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

            primitives.push_back(static_cast<int>(bp.meshes.size()));
            bp.meshes.push_back(std::move(mesh));
        }

        meshCache.emplace(meshIdx, std::move(primitives));
        return meshCache.at(meshIdx);
    };

    const auto& srcScene = model.defaultScene >= 0 ? model.scenes[model.defaultScene] : model.scenes[0];
    bp.name = !srcScene.name.empty() ? srcScene.name : filePath.stem().string();

    // Flatten the node tree into parent-indexed EntityBlueprints, preserving
    // local TRS (decomposing only matrix-authored nodes).
    std::function<void(int, int)> processNode = [&](int nodeIdx, int parentIndex) {
        const auto& srcNode = model.nodes[nodeIdx];
        Vapor::EntityBlueprint e;
        e.name = !srcNode.name.empty() ? srcNode.name : fmt::format("node_{}", nodeIdx);
        e.parent = parentIndex;
        if (!srcNode.matrix.empty()) {
            const glm::mat4 m(
                srcNode.matrix[0], srcNode.matrix[1], srcNode.matrix[2], srcNode.matrix[3],
                srcNode.matrix[4], srcNode.matrix[5], srcNode.matrix[6], srcNode.matrix[7],
                srcNode.matrix[8], srcNode.matrix[9], srcNode.matrix[10], srcNode.matrix[11],
                srcNode.matrix[12], srcNode.matrix[13], srcNode.matrix[14], srcNode.matrix[15]
            );
            Vapor::decomposeTransform(m, e.position, e.rotation, e.scale);
        } else {
            if (!srcNode.translation.empty())
                e.position = glm::vec3(srcNode.translation[0], srcNode.translation[1], srcNode.translation[2]);
            if (!srcNode.rotation.empty())
                e.rotation = glm::quat(
                    static_cast<float>(srcNode.rotation[3]),
                    static_cast<float>(srcNode.rotation[0]),
                    static_cast<float>(srcNode.rotation[1]),
                    static_cast<float>(srcNode.rotation[2])
                );
            if (!srcNode.scale.empty())
                e.scale = glm::vec3(srcNode.scale[0], srcNode.scale[1], srcNode.scale[2]);
        }
        if (srcNode.mesh >= 0) e.meshes = processMesh(srcNode.mesh);
        if (srcNode.light >= 0 && srcNode.light < static_cast<int>(bp.lights.size()))
            e.lights.push_back(srcNode.light);

        const int selfIndex = static_cast<int>(bp.entities.size());
        bp.entities.push_back(std::move(e));
        for (int childIdx : srcNode.children)
            processNode(childIdx, selfIndex);
    };

    for (int nodeIdx : srcScene.nodes)
        processNode(nodeIdx, -1);

    bp.ok = true;
    fmt::print(
        "loadGLTF '{}': {} entities, {} meshes, {} materials, {} lights\n",
        filename,
        bp.entities.size(),
        bp.meshes.size(),
        bp.materials.size(),
        bp.lights.size()
    );
    return bp;
}

auto AssetManager::loadModel(const std::string& filename) -> Vapor::SceneBlueprint {
    std::filesystem::path p(filename);
    const std::string ext = p.extension().string();
    if (ext == ".gltf" || ext == ".glb") return loadGLTF(filename);
    if (ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz") return loadUSD(filename);
    fmt::print("loadModel: unsupported model format '{}' ({})\n", ext, filename);
    return {};
}
