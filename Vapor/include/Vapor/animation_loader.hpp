#pragma once

#include "animation.hpp"
#include "animator.hpp"
#include "graphics.hpp"

#include <memory>
#include <string>
#include <vector>

// Forward declaration
namespace tinygltf {
class Model;
struct Skin;
struct Animation;
}

namespace Vapor {

/**
 * @brief Result of loading a skinned model from GLTF.
 */
struct SkinnedModelData {
    std::shared_ptr<Skeleton> skeleton;
    std::vector<std::shared_ptr<AnimationClip>> animations;
    std::vector<std::shared_ptr<SkinnedMesh>> meshes;

    // Mapping from GLTF node index to skeleton joint index
    std::unordered_map<int, int> nodeToJoint;
};

/**
 * @brief Loader for skeletal animation data from GLTF files.
 *
 * This loader extracts:
 * - Skeleton hierarchy from GLTF skins
 * - Animation clips from GLTF animations
 * - Skinned mesh vertices with joint weights
 *
 * Designed to work with the existing AssetManager infrastructure.
 */
class AnimationLoader {
public:
    /**
     * @brief Load a complete skinned model from a GLTF file.
     * @param filename Path to the GLTF file
     * @return SkinnedModelData containing skeleton, animations, and meshes
     */
    static SkinnedModelData loadSkinnedModel(const std::string& filename);

    /**
     * @brief Load skeleton from a GLTF model.
     * @param model TinyGLTF model
     * @param skinIndex Index of the skin to load (-1 for first available)
     * @return Skeleton or nullptr on failure
     */
    static std::shared_ptr<Skeleton> loadSkeleton(const tinygltf::Model& model,
                                                   int skinIndex = -1);

    /**
     * @brief Load all animations from a GLTF model.
     * @param model TinyGLTF model
     * @param skeleton Target skeleton (animations will be remapped to joint indices)
     * @return Vector of animation clips
     */
    static std::vector<std::shared_ptr<AnimationClip>> loadAnimations(
        const tinygltf::Model& model,
        const Skeleton& skeleton);

    /**
     * @brief Load a single animation from a GLTF model.
     * @param model TinyGLTF model
     * @param animIndex Animation index
     * @param skeleton Target skeleton
     * @return Animation clip or nullptr on failure
     */
    static std::shared_ptr<AnimationClip> loadAnimation(
        const tinygltf::Model& model,
        int animIndex,
        const Skeleton& skeleton);

    /**
     * @brief Load skinned mesh data from a GLTF model.
     * @param model TinyGLTF model
     * @param meshIndex Mesh index
     * @param skeleton Target skeleton
     * @return Vector of skinned mesh primitives
     */
    static std::vector<std::shared_ptr<SkinnedMesh>> loadSkinnedMesh(
        const tinygltf::Model& model,
        int meshIndex,
        std::shared_ptr<Skeleton> skeleton);

private:
    // Helper to get accessor data
    template<typename T>
    static std::vector<T> getAccessorData(const tinygltf::Model& model, int accessorIndex);

    // Helper to build node-to-joint mapping
    static std::unordered_map<int, int> buildNodeToJointMap(
        const tinygltf::Model& model,
        const tinygltf::Skin& skin);
};

}  // namespace Vapor
