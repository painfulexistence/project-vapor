#include "animation_loader.hpp"

#include <SDL3/SDL_filesystem.h>
#include <fmt/core.h>
#include <tiny_gltf.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace Vapor {

// Helper to get local transform from a GLTF node
static glm::mat4 getLocalTransform(const tinygltf::Node& node) {
    if (!node.matrix.empty()) {
        return glm::mat4(
            node.matrix[0], node.matrix[1], node.matrix[2], node.matrix[3],
            node.matrix[4], node.matrix[5], node.matrix[6], node.matrix[7],
            node.matrix[8], node.matrix[9], node.matrix[10], node.matrix[11],
            node.matrix[12], node.matrix[13], node.matrix[14], node.matrix[15]
        );
    }

    glm::mat4 T = glm::mat4(1.0f);
    glm::mat4 R = glm::mat4(1.0f);
    glm::mat4 S = glm::mat4(1.0f);

    if (!node.translation.empty()) {
        T = glm::translate(glm::mat4(1.0f),
            glm::vec3(node.translation[0], node.translation[1], node.translation[2]));
    }
    if (!node.rotation.empty()) {
        glm::quat q(
            static_cast<float>(node.rotation[3]),  // w
            static_cast<float>(node.rotation[0]),  // x
            static_cast<float>(node.rotation[1]),  // y
            static_cast<float>(node.rotation[2])   // z
        );
        R = glm::mat4_cast(q);
    }
    if (!node.scale.empty()) {
        S = glm::scale(glm::mat4(1.0f),
            glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
    }

    return T * R * S;
}

// Template specializations for accessor data extraction
template<>
std::vector<float> AnimationLoader::getAccessorData(const tinygltf::Model& model, int accessorIndex) {
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size())) {
        return {};
    }

    const auto& accessor = model.accessors[accessorIndex];
    const auto& bufferView = model.bufferViews[accessor.bufferView];
    const auto& buffer = model.buffers[bufferView.buffer];

    std::vector<float> result;
    size_t componentCount = 1;

    switch (accessor.type) {
        case TINYGLTF_TYPE_SCALAR: componentCount = 1; break;
        case TINYGLTF_TYPE_VEC2: componentCount = 2; break;
        case TINYGLTF_TYPE_VEC3: componentCount = 3; break;
        case TINYGLTF_TYPE_VEC4: componentCount = 4; break;
        case TINYGLTF_TYPE_MAT4: componentCount = 16; break;
        default: return {};
    }

    result.resize(accessor.count * componentCount);

    const unsigned char* dataPtr = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
    size_t stride = bufferView.byteStride ? bufferView.byteStride : componentCount * sizeof(float);

    for (size_t i = 0; i < accessor.count; ++i) {
        const float* srcPtr = reinterpret_cast<const float*>(dataPtr + i * stride);
        for (size_t j = 0; j < componentCount; ++j) {
            result[i * componentCount + j] = srcPtr[j];
        }
    }

    return result;
}

template<>
std::vector<glm::mat4> AnimationLoader::getAccessorData(const tinygltf::Model& model, int accessorIndex) {
    auto floatData = getAccessorData<float>(model, accessorIndex);
    std::vector<glm::mat4> result;
    result.reserve(floatData.size() / 16);

    for (size_t i = 0; i + 15 < floatData.size(); i += 16) {
        result.push_back(glm::make_mat4(&floatData[i]));
    }

    return result;
}

std::unordered_map<int, int> AnimationLoader::buildNodeToJointMap(
    const tinygltf::Model& model,
    const tinygltf::Skin& skin)
{
    std::unordered_map<int, int> map;
    for (size_t i = 0; i < skin.joints.size(); ++i) {
        map[skin.joints[i]] = static_cast<int>(i);
    }
    return map;
}

SkinnedModelData AnimationLoader::loadSkinnedModel(const std::string& filename) {
    SkinnedModelData result;

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    std::string fullPath = SDL_GetBasePath() + filename;
    bool loaded = false;

    if (filename.ends_with(".glb")) {
        loaded = loader.LoadBinaryFromFile(&model, &err, &warn, fullPath);
    } else {
        loaded = loader.LoadASCIIFromFile(&model, &err, &warn, fullPath);
    }

    if (!warn.empty()) {
        fmt::print("GLTF Warning: {}\n", warn);
    }
    if (!err.empty()) {
        fmt::print("GLTF Error: {}\n", err);
    }
    if (!loaded) {
        fmt::print("Failed to load GLTF: {}\n", filename);
        return result;
    }

    // Load skeleton from first available skin
    if (!model.skins.empty()) {
        result.skeleton = loadSkeleton(model, 0);
        if (result.skeleton) {
            // Build node-to-joint mapping
            result.nodeToJoint = buildNodeToJointMap(model, model.skins[0]);

            // Load animations
            result.animations = loadAnimations(model, *result.skeleton);

            // Load skinned meshes
            for (const auto& node : model.nodes) {
                if (node.mesh >= 0 && node.skin >= 0) {
                    auto meshes = loadSkinnedMesh(model, node.mesh, result.skeleton);
                    result.meshes.insert(result.meshes.end(), meshes.begin(), meshes.end());
                }
            }
        }
    }

    fmt::print("Loaded skinned model: {} joints, {} animations, {} meshes\n",
               result.skeleton ? result.skeleton->getJointCount() : 0,
               result.animations.size(),
               result.meshes.size());

    return result;
}

std::shared_ptr<Skeleton> AnimationLoader::loadSkeleton(
    const tinygltf::Model& model,
    int skinIndex)
{
    if (model.skins.empty()) {
        fmt::print("AnimationLoader: No skins in model\n");
        return nullptr;
    }

    if (skinIndex < 0) skinIndex = 0;
    if (skinIndex >= static_cast<int>(model.skins.size())) {
        fmt::print("AnimationLoader: Invalid skin index {}\n", skinIndex);
        return nullptr;
    }

    const auto& skin = model.skins[skinIndex];
    if (skin.joints.empty()) {
        fmt::print("AnimationLoader: Skin has no joints\n");
        return nullptr;
    }

    // Load inverse bind matrices
    std::vector<glm::mat4> inverseBindMatrices;
    if (skin.inverseBindMatrices >= 0) {
        inverseBindMatrices = getAccessorData<glm::mat4>(model, skin.inverseBindMatrices);
    }

    // Ensure we have enough inverse bind matrices
    if (inverseBindMatrices.size() < skin.joints.size()) {
        inverseBindMatrices.resize(skin.joints.size(), glm::mat4(1.0f));
    }

    // Build node-to-joint mapping for finding parent indices
    std::unordered_map<int, int> nodeToJoint;
    for (size_t i = 0; i < skin.joints.size(); ++i) {
        nodeToJoint[skin.joints[i]] = static_cast<int>(i);
    }

    // Build joint hierarchy
    std::vector<Joint> joints;
    joints.reserve(skin.joints.size());

    for (size_t i = 0; i < skin.joints.size(); ++i) {
        int nodeIndex = skin.joints[i];
        const auto& node = model.nodes[nodeIndex];

        Joint joint;
        joint.name = node.name.empty() ? fmt::format("joint_{}", i) : node.name;
        joint.inverseBindMatrix = inverseBindMatrices[i];
        joint.localBindPose = getLocalTransform(node);

        // Find parent joint
        joint.parentIndex = -1;
        for (size_t j = 0; j < model.nodes.size(); ++j) {
            const auto& potentialParent = model.nodes[j];
            for (int childIdx : potentialParent.children) {
                if (childIdx == nodeIndex) {
                    auto it = nodeToJoint.find(static_cast<int>(j));
                    if (it != nodeToJoint.end()) {
                        joint.parentIndex = it->second;
                    }
                    break;
                }
            }
            if (joint.parentIndex >= 0) break;
        }

        joints.push_back(std::move(joint));
    }

    // Create skeleton
    auto skeleton = std::make_shared<Skeleton>();
    if (!skeleton->initialize(joints)) {
        fmt::print("AnimationLoader: Failed to initialize skeleton\n");
        return nullptr;
    }

    fmt::print("AnimationLoader: Loaded skeleton with {} joints\n", joints.size());
    return skeleton;
}

std::vector<std::shared_ptr<AnimationClip>> AnimationLoader::loadAnimations(
    const tinygltf::Model& model,
    const Skeleton& skeleton)
{
    std::vector<std::shared_ptr<AnimationClip>> clips;
    clips.reserve(model.animations.size());

    for (size_t i = 0; i < model.animations.size(); ++i) {
        auto clip = loadAnimation(model, static_cast<int>(i), skeleton);
        if (clip) {
            clips.push_back(std::move(clip));
        }
    }

    return clips;
}

std::shared_ptr<AnimationClip> AnimationLoader::loadAnimation(
    const tinygltf::Model& model,
    int animIndex,
    const Skeleton& skeleton)
{
    if (animIndex < 0 || animIndex >= static_cast<int>(model.animations.size())) {
        return nullptr;
    }

    const auto& gltfAnim = model.animations[animIndex];
    std::string name = gltfAnim.name.empty() ? fmt::format("animation_{}", animIndex) : gltfAnim.name;

    // Build node-to-joint mapping from skin (assuming first skin)
    std::unordered_map<int, int> nodeToJoint;
    if (!model.skins.empty()) {
        const auto& skin = model.skins[0];
        for (size_t i = 0; i < skin.joints.size(); ++i) {
            nodeToJoint[skin.joints[i]] = static_cast<int>(i);
        }
    }

    std::vector<AnimationClip::Channel> channels;
    channels.reserve(gltfAnim.channels.size());

    for (const auto& gltfChannel : gltfAnim.channels) {
        int targetNode = gltfChannel.target_node;
        if (targetNode < 0) continue;

        auto jointIt = nodeToJoint.find(targetNode);
        if (jointIt == nodeToJoint.end()) continue;

        int targetJoint = jointIt->second;

        AnimationClip::Channel channel;
        channel.targetJoint = targetJoint;

        // Determine path
        const std::string& pathStr = gltfChannel.target_path;
        if (pathStr == "translation") {
            channel.path = AnimationClip::Channel::Path::Translation;
        } else if (pathStr == "rotation") {
            channel.path = AnimationClip::Channel::Path::Rotation;
        } else if (pathStr == "scale") {
            channel.path = AnimationClip::Channel::Path::Scale;
        } else {
            continue;  // Unsupported path (weights, etc.)
        }

        // Get sampler
        if (gltfChannel.sampler < 0 || gltfChannel.sampler >= static_cast<int>(gltfAnim.samplers.size())) {
            continue;
        }
        const auto& sampler = gltfAnim.samplers[gltfChannel.sampler];

        // Determine interpolation
        if (sampler.interpolation == "STEP") {
            channel.interpolation = AnimationClip::Channel::Interpolation::Step;
        } else if (sampler.interpolation == "LINEAR") {
            channel.interpolation = AnimationClip::Channel::Interpolation::Linear;
        } else if (sampler.interpolation == "CUBICSPLINE") {
            channel.interpolation = AnimationClip::Channel::Interpolation::CubicSpline;
        }

        // Load timestamps
        channel.timestamps = getAccessorData<float>(model, sampler.input);
        if (channel.timestamps.empty()) continue;

        // Load values
        channel.values = getAccessorData<float>(model, sampler.output);
        if (channel.values.empty()) continue;

        channels.push_back(std::move(channel));
    }

    if (channels.empty()) {
        fmt::print("AnimationLoader: Animation '{}' has no valid channels\n", name);
        return nullptr;
    }

    auto clip = std::make_shared<AnimationClip>();
    if (!clip->initialize(name, channels, skeleton)) {
        fmt::print("AnimationLoader: Failed to initialize animation '{}'\n", name);
        return nullptr;
    }

    return clip;
}

std::vector<std::shared_ptr<SkinnedMesh>> AnimationLoader::loadSkinnedMesh(
    const tinygltf::Model& model,
    int meshIndex,
    std::shared_ptr<Skeleton> skeleton)
{
    std::vector<std::shared_ptr<SkinnedMesh>> meshes;

    if (meshIndex < 0 || meshIndex >= static_cast<int>(model.meshes.size())) {
        return meshes;
    }

    const auto& gltfMesh = model.meshes[meshIndex];

    for (const auto& primitive : gltfMesh.primitives) {
        auto mesh = std::make_shared<SkinnedMesh>();
        mesh->skeleton = skeleton;

        // Check for required attributes
        auto posIt = primitive.attributes.find("POSITION");
        if (posIt == primitive.attributes.end()) continue;

        auto jointsIt = primitive.attributes.find("JOINTS_0");
        auto weightsIt = primitive.attributes.find("WEIGHTS_0");

        if (jointsIt == primitive.attributes.end() || weightsIt == primitive.attributes.end()) {
            fmt::print("AnimationLoader: Mesh primitive missing JOINTS_0 or WEIGHTS_0\n");
            continue;
        }

        mesh->hasPosition = true;
        mesh->hasJoints = true;
        mesh->hasWeights = true;

        // Get vertex count
        const auto& posAccessor = model.accessors[posIt->second];
        size_t vertexCount = posAccessor.count;
        mesh->vertices.resize(vertexCount);

        // Load positions
        {
            const auto& accessor = posAccessor;
            const auto& bufferView = model.bufferViews[accessor.bufferView];
            const auto& buffer = model.buffers[bufferView.buffer];
            const float* data = reinterpret_cast<const float*>(
                buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
            size_t stride = bufferView.byteStride ? bufferView.byteStride / sizeof(float) : 3;

            for (size_t i = 0; i < vertexCount; ++i) {
                mesh->vertices[i].position = glm::vec3(
                    data[i * stride + 0],
                    data[i * stride + 1],
                    data[i * stride + 2]
                );
            }
        }

        // Load normals
        auto normIt = primitive.attributes.find("NORMAL");
        if (normIt != primitive.attributes.end()) {
            mesh->hasNormal = true;
            const auto& accessor = model.accessors[normIt->second];
            const auto& bufferView = model.bufferViews[accessor.bufferView];
            const auto& buffer = model.buffers[bufferView.buffer];
            const float* data = reinterpret_cast<const float*>(
                buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
            size_t stride = bufferView.byteStride ? bufferView.byteStride / sizeof(float) : 3;

            for (size_t i = 0; i < vertexCount; ++i) {
                mesh->vertices[i].normal = glm::vec3(
                    data[i * stride + 0],
                    data[i * stride + 1],
                    data[i * stride + 2]
                );
            }
        }

        // Load tangents
        auto tanIt = primitive.attributes.find("TANGENT");
        if (tanIt != primitive.attributes.end()) {
            mesh->hasTangent = true;
            const auto& accessor = model.accessors[tanIt->second];
            const auto& bufferView = model.bufferViews[accessor.bufferView];
            const auto& buffer = model.buffers[bufferView.buffer];
            const float* data = reinterpret_cast<const float*>(
                buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
            size_t stride = bufferView.byteStride ? bufferView.byteStride / sizeof(float) : 4;

            for (size_t i = 0; i < vertexCount; ++i) {
                mesh->vertices[i].tangent = glm::vec4(
                    data[i * stride + 0],
                    data[i * stride + 1],
                    data[i * stride + 2],
                    data[i * stride + 3]
                );
            }
        }

        // Load UVs
        auto uvIt = primitive.attributes.find("TEXCOORD_0");
        if (uvIt != primitive.attributes.end()) {
            mesh->hasUV0 = true;
            const auto& accessor = model.accessors[uvIt->second];
            const auto& bufferView = model.bufferViews[accessor.bufferView];
            const auto& buffer = model.buffers[bufferView.buffer];
            const float* data = reinterpret_cast<const float*>(
                buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
            size_t stride = bufferView.byteStride ? bufferView.byteStride / sizeof(float) : 2;

            for (size_t i = 0; i < vertexCount; ++i) {
                mesh->vertices[i].uv = glm::vec2(
                    data[i * stride + 0],
                    data[i * stride + 1]
                );
            }
        }

        // Load joint indices
        {
            const auto& accessor = model.accessors[jointsIt->second];
            const auto& bufferView = model.bufferViews[accessor.bufferView];
            const auto& buffer = model.buffers[bufferView.buffer];

            // Joint indices can be UNSIGNED_BYTE or UNSIGNED_SHORT
            if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                const uint8_t* data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
                size_t stride = bufferView.byteStride ? bufferView.byteStride : 4;

                for (size_t i = 0; i < vertexCount; ++i) {
                    const uint8_t* ptr = data + i * stride;
                    mesh->vertices[i].jointIndices = glm::uvec4(ptr[0], ptr[1], ptr[2], ptr[3]);
                }
            } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                const uint16_t* data = reinterpret_cast<const uint16_t*>(
                    buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
                size_t stride = bufferView.byteStride ? bufferView.byteStride / sizeof(uint16_t) : 4;

                for (size_t i = 0; i < vertexCount; ++i) {
                    mesh->vertices[i].jointIndices = glm::uvec4(
                        data[i * stride + 0],
                        data[i * stride + 1],
                        data[i * stride + 2],
                        data[i * stride + 3]
                    );
                }
            }
        }

        // Load joint weights
        {
            const auto& accessor = model.accessors[weightsIt->second];
            const auto& bufferView = model.bufferViews[accessor.bufferView];
            const auto& buffer = model.buffers[bufferView.buffer];
            const float* data = reinterpret_cast<const float*>(
                buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
            size_t stride = bufferView.byteStride ? bufferView.byteStride / sizeof(float) : 4;

            for (size_t i = 0; i < vertexCount; ++i) {
                mesh->vertices[i].jointWeights = glm::vec4(
                    data[i * stride + 0],
                    data[i * stride + 1],
                    data[i * stride + 2],
                    data[i * stride + 3]
                );
            }
        }

        // Load indices
        if (primitive.indices >= 0) {
            const auto& accessor = model.accessors[primitive.indices];
            const auto& bufferView = model.bufferViews[accessor.bufferView];
            const auto& buffer = model.buffers[bufferView.buffer];

            mesh->indices.resize(accessor.count);

            switch (accessor.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
                    const uint8_t* data = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
                    for (size_t i = 0; i < accessor.count; ++i) {
                        mesh->indices[i] = data[i];
                    }
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
                    const uint16_t* data = reinterpret_cast<const uint16_t*>(
                        buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
                    for (size_t i = 0; i < accessor.count; ++i) {
                        mesh->indices[i] = data[i];
                    }
                    break;
                }
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
                    const uint32_t* data = reinterpret_cast<const uint32_t*>(
                        buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
                    for (size_t i = 0; i < accessor.count; ++i) {
                        mesh->indices[i] = data[i];
                    }
                    break;
                }
            }
        }

        // Calculate AABB
        mesh->calculateLocalAABB();

        mesh->vertexCount = static_cast<Uint32>(mesh->vertices.size());
        mesh->indexCount = static_cast<Uint32>(mesh->indices.size());

        meshes.push_back(std::move(mesh));
    }

    return meshes;
}

}  // namespace Vapor
