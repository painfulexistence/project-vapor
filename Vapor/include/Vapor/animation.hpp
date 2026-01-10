#pragma once

#include <SDL3/SDL_stdinc.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

// Forward declarations for ozz types
namespace ozz {
namespace animation {
class Skeleton;
class Animation;
}  // namespace animation
}  // namespace ozz

namespace Vapor {

// Maximum bones per skeleton (must match shader constant)
constexpr Uint32 MAX_BONES_PER_SKELETON = 256;

// Maximum joints influencing a single vertex
constexpr Uint32 MAX_JOINTS_PER_VERTEX = 4;

/**
 * @brief Joint/Bone data for a single joint in the skeleton hierarchy.
 *
 * This is a lightweight representation used for CPU-side operations.
 * The actual runtime uses ozz::animation::Skeleton for optimized sampling.
 */
struct Joint {
    std::string name;
    int parentIndex = -1;  // -1 for root joints
    glm::mat4 inverseBindMatrix = glm::mat4(1.0f);
    glm::mat4 localBindPose = glm::mat4(1.0f);
};

/**
 * @brief Skeleton definition - shared across all instances using this skeleton.
 *
 * Design for crowd systems:
 * - Skeleton data is immutable after loading
 * - Multiple AnimationInstance objects can reference the same Skeleton
 * - Contains ozz skeleton for optimized runtime sampling
 */
class Skeleton {
public:
    Skeleton() = default;
    ~Skeleton();

    // Non-copyable, movable
    Skeleton(const Skeleton&) = delete;
    Skeleton& operator=(const Skeleton&) = delete;
    Skeleton(Skeleton&&) noexcept;
    Skeleton& operator=(Skeleton&&) noexcept;

    /**
     * @brief Initialize from GLTF joint data.
     * @param joints Vector of joint definitions
     * @return true if initialization succeeded
     */
    bool initialize(const std::vector<Joint>& joints);

    /**
     * @brief Get the number of joints in this skeleton.
     */
    Uint32 getJointCount() const { return static_cast<Uint32>(joints.size()); }

    /**
     * @brief Get joint by index.
     */
    const Joint& getJoint(Uint32 index) const { return joints[index]; }

    /**
     * @brief Find joint index by name.
     * @return Joint index or -1 if not found
     */
    int findJointIndex(const std::string& name) const;

    /**
     * @brief Get the ozz skeleton for runtime sampling.
     */
    ozz::animation::Skeleton* getOzzSkeleton() const { return ozzSkeleton.get(); }

    /**
     * @brief Get inverse bind matrices array (for GPU upload).
     */
    const std::vector<glm::mat4>& getInverseBindMatrices() const { return inverseBindMatrices; }

private:
    std::vector<Joint> joints;
    std::vector<glm::mat4> inverseBindMatrices;
    std::unordered_map<std::string, int> jointNameToIndex;
    std::unique_ptr<ozz::animation::Skeleton> ozzSkeleton;
};

/**
 * @brief Animation clip data - shared across all instances playing this animation.
 *
 * Design for crowd systems:
 * - Animation data is immutable after loading
 * - Multiple AnimationInstance objects can sample from the same clip
 * - Contains ozz animation for optimized runtime sampling
 */
class AnimationClip {
public:
    AnimationClip() = default;
    ~AnimationClip();

    // Non-copyable, movable
    AnimationClip(const AnimationClip&) = delete;
    AnimationClip& operator=(const AnimationClip&) = delete;
    AnimationClip(AnimationClip&&) noexcept;
    AnimationClip& operator=(AnimationClip&&) noexcept;

    /**
     * @brief Animation channel targeting a specific joint property.
     */
    struct Channel {
        enum class Path {
            Translation,
            Rotation,
            Scale
        };

        enum class Interpolation {
            Step,
            Linear,
            CubicSpline
        };

        int targetJoint = -1;
        Path path = Path::Translation;
        Interpolation interpolation = Interpolation::Linear;
        std::vector<float> timestamps;
        std::vector<float> values;  // Packed: vec3 for T/S, vec4 for R
    };

    /**
     * @brief Initialize from GLTF animation data.
     * @param name Animation name
     * @param channels Animation channels
     * @param skeleton Target skeleton (needed to build ozz animation)
     * @return true if initialization succeeded
     */
    bool initialize(const std::string& name,
                    const std::vector<Channel>& channels,
                    const Skeleton& skeleton);

    const std::string& getName() const { return name; }
    float getDuration() const { return duration; }

    /**
     * @brief Get the ozz animation for runtime sampling.
     */
    ozz::animation::Animation* getOzzAnimation() const { return ozzAnimation.get(); }

private:
    std::string name;
    float duration = 0.0f;
    std::vector<Channel> channels;
    std::unique_ptr<ozz::animation::Animation> ozzAnimation;
};

/**
 * @brief Skinned vertex data with joint influences.
 *
 * This extends the base VertexData with skeletal animation support.
 * Designed for GPU skinning with up to 4 joint influences per vertex.
 */
struct SkinnedVertexData {
    glm::vec3 position;
    glm::vec2 uv;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::uvec4 jointIndices;  // Indices into bone matrix palette
    glm::vec4 jointWeights;   // Blend weights (should sum to 1.0)
};

/**
 * @brief GPU-aligned bone matrix data for a single skeleton instance.
 *
 * This is the data uploaded to GPU for skinning.
 * For crowd systems, multiple of these can be batched into a single buffer.
 */
struct alignas(16) BoneMatrixPalette {
    glm::mat4 matrices[MAX_BONES_PER_SKELETON];
};

/**
 * @brief Per-instance animation state.
 *
 * This holds the runtime state for a single animated entity.
 * Designed to be lightweight for crowd systems where thousands
 * of instances may exist.
 */
struct AnimationState {
    float currentTime = 0.0f;
    float playbackSpeed = 1.0f;
    bool isPlaying = false;
    bool isLooping = true;

    // Blending support
    float blendWeight = 1.0f;
    float blendTime = 0.0f;
    float blendDuration = 0.0f;
};

/**
 * @brief Skinned mesh data associated with a skeleton.
 */
struct SkinnedMesh {
    std::vector<SkinnedVertexData> vertices;
    std::vector<Uint32> indices;
    std::shared_ptr<Skeleton> skeleton;

    // GPU buffer handles (set by renderer)
    Uint32 vertexBufferHandle = UINT32_MAX;
    Uint32 indexBufferHandle = UINT32_MAX;

    // Offsets for GPU-driven rendering
    Uint32 vertexOffset = 0;
    Uint32 indexOffset = 0;
    Uint32 vertexCount = 0;
    Uint32 indexCount = 0;
};

}  // namespace Vapor
