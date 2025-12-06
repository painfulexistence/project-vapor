#pragma once

#include "animation.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations for ozz types
namespace ozz {
namespace animation {
class SamplingJob;
struct SamplingCache;
}  // namespace animation
namespace math {
struct SoaTransform;
}  // namespace math
}  // namespace ozz

namespace Vapor {

/**
 * @brief Animation layer for blending multiple animations.
 *
 * Supports additive and override blend modes.
 */
struct AnimationLayer {
    enum class BlendMode {
        Override,   // Replace lower layers
        Additive    // Add to lower layers
    };

    std::shared_ptr<AnimationClip> clip;
    AnimationState state;
    BlendMode blendMode = BlendMode::Override;
    float weight = 1.0f;

    // Mask for selective joint blending (empty = all joints)
    std::vector<float> jointMask;
};

/**
 * @brief Animator - per-instance animation controller.
 *
 * This class handles animation playback, blending, and bone matrix computation
 * for a single animated entity. It uses ozz-animation for efficient sampling.
 *
 * Design for crowd systems:
 * - Animator instances are lightweight, referencing shared Skeleton/AnimationClip data
 * - Bone matrices are computed lazily and can be batched for GPU upload
 * - Multiple Animators can be updated in parallel
 */
class Animator {
public:
    Animator();
    ~Animator();

    // Non-copyable, movable
    Animator(const Animator&) = delete;
    Animator& operator=(const Animator&) = delete;
    Animator(Animator&&) noexcept;
    Animator& operator=(Animator&&) noexcept;

    /**
     * @brief Initialize with a skeleton.
     * @param skeleton Shared skeleton definition
     * @return true if initialization succeeded
     */
    bool initialize(std::shared_ptr<Skeleton> skeleton);

    /**
     * @brief Add an animation clip to the animator's library.
     * @param clip Animation clip to add
     */
    void addAnimation(std::shared_ptr<AnimationClip> clip);

    /**
     * @brief Play an animation by name.
     * @param name Animation name
     * @param loop Whether to loop the animation
     * @param blendTime Crossfade duration in seconds (0 for instant switch)
     * @return true if animation was found and started
     */
    bool play(const std::string& name, bool loop = true, float blendTime = 0.0f);

    /**
     * @brief Play an animation on a specific layer.
     * @param layerIndex Layer index (0 = base layer)
     * @param name Animation name
     * @param loop Whether to loop
     * @param blendTime Crossfade duration
     * @return true if successful
     */
    bool playOnLayer(Uint32 layerIndex, const std::string& name,
                     bool loop = true, float blendTime = 0.0f);

    /**
     * @brief Stop all animations.
     */
    void stop();

    /**
     * @brief Stop animation on a specific layer.
     */
    void stopLayer(Uint32 layerIndex);

    /**
     * @brief Pause/unpause playback.
     */
    void setPaused(bool paused);

    /**
     * @brief Set playback speed (1.0 = normal, 0.5 = half speed, etc.)
     */
    void setPlaybackSpeed(float speed);

    /**
     * @brief Set layer blend weight.
     */
    void setLayerWeight(Uint32 layerIndex, float weight);

    /**
     * @brief Update animation state and compute bone matrices.
     * @param deltaTime Time elapsed since last update
     */
    void update(float deltaTime);

    /**
     * @brief Get computed bone matrices for GPU skinning.
     *
     * These matrices are: inverseBindMatrix * worldTransform * animatedLocalTransform
     * Ready for direct use in vertex skinning.
     */
    const std::vector<glm::mat4>& getBoneMatrices() const { return boneMatrices; }

    /**
     * @brief Get the skeleton.
     */
    std::shared_ptr<Skeleton> getSkeleton() const { return skeleton; }

    /**
     * @brief Check if any animation is currently playing.
     */
    bool isPlaying() const;

    /**
     * @brief Get current animation name (base layer).
     */
    const std::string& getCurrentAnimationName() const;

    /**
     * @brief Get current playback time (base layer).
     */
    float getCurrentTime() const;

    /**
     * @brief Get normalized playback progress (0-1) for base layer.
     */
    float getProgress() const;

    /**
     * @brief Event callback type.
     */
    using EventCallback = std::function<void(const std::string& eventName)>;

    /**
     * @brief Set callback for animation events.
     */
    void setEventCallback(EventCallback callback) { eventCallback = std::move(callback); }

    /**
     * @brief Get the number of animation layers.
     */
    Uint32 getLayerCount() const { return static_cast<Uint32>(layers.size()); }

    /**
     * @brief Ensure at least N layers exist.
     */
    void ensureLayers(Uint32 count);

private:
    void sampleAnimation(const AnimationLayer& layer, std::vector<ozz::math::SoaTransform>& locals);
    void blendLayers();
    void computeBoneMatrices();

    std::shared_ptr<Skeleton> skeleton;
    std::unordered_map<std::string, std::shared_ptr<AnimationClip>> animations;

    std::vector<AnimationLayer> layers;
    std::vector<glm::mat4> boneMatrices;

    // ozz runtime data
    struct OzzRuntimeData;
    std::unique_ptr<OzzRuntimeData> ozzData;

    bool isPaused = false;
    float globalPlaybackSpeed = 1.0f;
    EventCallback eventCallback;
};

/**
 * @brief Batch animator for crowd systems.
 *
 * Efficiently updates multiple animators sharing the same skeleton
 * and uploads bone matrices to GPU in a single batch.
 */
class AnimatorBatch {
public:
    AnimatorBatch();
    ~AnimatorBatch();

    /**
     * @brief Initialize for a specific skeleton.
     * @param skeleton Shared skeleton for all instances
     * @param maxInstances Maximum number of instances to support
     */
    bool initialize(std::shared_ptr<Skeleton> skeleton, Uint32 maxInstances);

    /**
     * @brief Add an animator to the batch.
     * @return Instance index, or UINT32_MAX if batch is full
     */
    Uint32 addInstance();

    /**
     * @brief Remove an animator from the batch.
     */
    void removeInstance(Uint32 index);

    /**
     * @brief Get animator for an instance.
     */
    Animator* getAnimator(Uint32 index);

    /**
     * @brief Update all animators in parallel.
     * @param deltaTime Time elapsed since last update
     */
    void updateAll(float deltaTime);

    /**
     * @brief Get the packed bone matrices buffer for GPU upload.
     *
     * Layout: [Instance0 Bones][Instance1 Bones]...[InstanceN Bones]
     * Each instance has skeleton->getJointCount() matrices.
     */
    const std::vector<glm::mat4>& getPackedBoneMatrices() const { return packedBoneMatrices; }

    /**
     * @brief Get bone matrix offset for an instance (in number of mat4s).
     */
    Uint32 getBoneMatrixOffset(Uint32 instanceIndex) const;

    /**
     * @brief Get current instance count.
     */
    Uint32 getInstanceCount() const { return static_cast<Uint32>(animators.size()); }

private:
    std::shared_ptr<Skeleton> skeleton;
    std::vector<std::unique_ptr<Animator>> animators;
    std::vector<Uint32> freeIndices;
    std::vector<glm::mat4> packedBoneMatrices;
    Uint32 maxInstances = 0;
};

}  // namespace Vapor
