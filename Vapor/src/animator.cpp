#include "animator.hpp"

#include <fmt/core.h>
#include <glm/gtc/matrix_transform.hpp>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/containers/vector.h>

namespace Vapor {

// ============================================================================
// Animator Implementation
// ============================================================================

struct Animator::OzzRuntimeData {
    // Sampling cache per layer
    std::vector<ozz::animation::SamplingJob::Context> samplingContexts;

    // Local transforms buffer (SoA format for SIMD)
    ozz::vector<ozz::math::SoaTransform> locals;

    // Blending buffers
    std::vector<ozz::vector<ozz::math::SoaTransform>> layerLocals;

    // Model-space transforms
    ozz::vector<ozz::math::Float4x4> models;
};

Animator::Animator() : ozzData(std::make_unique<OzzRuntimeData>()) {
    // Create default base layer
    layers.emplace_back();
}

Animator::~Animator() = default;

Animator::Animator(Animator&&) noexcept = default;
Animator& Animator::operator=(Animator&&) noexcept = default;

bool Animator::initialize(std::shared_ptr<Skeleton> skel) {
    if (!skel || !skel->getOzzSkeleton()) {
        fmt::print("Animator::initialize: Invalid skeleton\n");
        return false;
    }

    skeleton = std::move(skel);

    const ozz::animation::Skeleton* ozzSkel = skeleton->getOzzSkeleton();
    int numJoints = ozzSkel->num_joints();
    int numSoaJoints = ozzSkel->num_soa_joints();

    // Allocate ozz buffers
    ozzData->locals.resize(numSoaJoints);
    ozzData->models.resize(numJoints);

    // Allocate bone matrices
    boneMatrices.resize(skeleton->getJointCount(), glm::mat4(1.0f));

    // Initialize layer buffers
    ensureLayers(1);

    fmt::print("Animator::initialize: Ready with {} joints\n", numJoints);
    return true;
}

void Animator::addAnimation(std::shared_ptr<AnimationClip> clip) {
    if (!clip) return;
    animations[clip->getName()] = std::move(clip);
}

bool Animator::play(const std::string& name, bool loop, float blendTime) {
    return playOnLayer(0, name, loop, blendTime);
}

bool Animator::playOnLayer(Uint32 layerIndex, const std::string& name,
                            bool loop, float blendTime) {
    auto it = animations.find(name);
    if (it == animations.end()) {
        fmt::print("Animator::play: Animation '{}' not found\n", name);
        return false;
    }

    ensureLayers(layerIndex + 1);

    auto& layer = layers[layerIndex];
    auto& newClip = it->second;

    // Setup blending if requested and there's a current animation
    if (blendTime > 0.0f && layer.clip) {
        layer.state.blendTime = 0.0f;
        layer.state.blendDuration = blendTime;
    } else {
        layer.state.blendTime = 0.0f;
        layer.state.blendDuration = 0.0f;
    }

    layer.clip = newClip;
    layer.state.currentTime = 0.0f;
    layer.state.isPlaying = true;
    layer.state.isLooping = loop;

    return true;
}

void Animator::stop() {
    for (auto& layer : layers) {
        layer.state.isPlaying = false;
        layer.state.currentTime = 0.0f;
    }
}

void Animator::stopLayer(Uint32 layerIndex) {
    if (layerIndex < layers.size()) {
        layers[layerIndex].state.isPlaying = false;
        layers[layerIndex].state.currentTime = 0.0f;
    }
}

void Animator::setPaused(bool paused) {
    isPaused = paused;
}

void Animator::setPlaybackSpeed(float speed) {
    globalPlaybackSpeed = speed;
}

void Animator::setLayerWeight(Uint32 layerIndex, float weight) {
    if (layerIndex < layers.size()) {
        layers[layerIndex].weight = weight;
    }
}

void Animator::ensureLayers(Uint32 count) {
    while (layers.size() < count) {
        layers.emplace_back();
    }

    // Ensure ozz buffers for each layer
    const ozz::animation::Skeleton* ozzSkel = skeleton ? skeleton->getOzzSkeleton() : nullptr;
    if (ozzSkel) {
        int numSoaJoints = ozzSkel->num_soa_joints();

        while (ozzData->samplingContexts.size() < count) {
            ozzData->samplingContexts.emplace_back();
            ozzData->samplingContexts.back().Resize(ozzSkel->num_joints());
        }

        while (ozzData->layerLocals.size() < count) {
            ozzData->layerLocals.emplace_back();
            ozzData->layerLocals.back().resize(numSoaJoints);
        }
    }
}

void Animator::update(float deltaTime) {
    if (!skeleton || isPaused) return;

    float effectiveDt = deltaTime * globalPlaybackSpeed;

    // Update animation times for each layer
    for (auto& layer : layers) {
        if (!layer.state.isPlaying || !layer.clip) continue;

        float speed = layer.state.playbackSpeed;
        layer.state.currentTime += effectiveDt * speed;

        float duration = layer.clip->getDuration();
        if (duration > 0.0f) {
            if (layer.state.isLooping) {
                while (layer.state.currentTime >= duration) {
                    layer.state.currentTime -= duration;
                }
                while (layer.state.currentTime < 0.0f) {
                    layer.state.currentTime += duration;
                }
            } else {
                if (layer.state.currentTime >= duration) {
                    layer.state.currentTime = duration;
                    layer.state.isPlaying = false;
                }
                if (layer.state.currentTime < 0.0f) {
                    layer.state.currentTime = 0.0f;
                    layer.state.isPlaying = false;
                }
            }
        }

        // Update blend transition
        if (layer.state.blendDuration > 0.0f) {
            layer.state.blendTime += effectiveDt;
            if (layer.state.blendTime >= layer.state.blendDuration) {
                layer.state.blendTime = layer.state.blendDuration;
                layer.state.blendDuration = 0.0f;
            }
        }
    }

    // Blend layers and compute bone matrices
    blendLayers();
    computeBoneMatrices();
}

void Animator::sampleAnimation(const AnimationLayer& layer,
                                std::vector<ozz::math::SoaTransform>& locals) {
    // This is a simplified version - full implementation would use ozz sampling
    // For now we just use the ozz sampling job
}

void Animator::blendLayers() {
    const ozz::animation::Skeleton* ozzSkel = skeleton->getOzzSkeleton();
    if (!ozzSkel) return;

    int numSoaJoints = ozzSkel->num_soa_joints();

    // Sample each active layer
    std::vector<ozz::animation::BlendingJob::Layer> blendLayers;

    for (size_t i = 0; i < layers.size(); ++i) {
        auto& layer = layers[i];
        if (!layer.clip || !layer.clip->getOzzAnimation()) continue;
        if (layer.weight <= 0.0f) continue;

        // Sample this layer's animation
        ozz::animation::SamplingJob samplingJob;
        samplingJob.animation = layer.clip->getOzzAnimation();
        samplingJob.context = &ozzData->samplingContexts[i];
        samplingJob.ratio = layer.state.currentTime / layer.clip->getDuration();
        samplingJob.output = ozz::make_span(ozzData->layerLocals[i]);

        if (!samplingJob.Run()) {
            fmt::print("Animator::blendLayers: Sampling failed for layer {}\n", i);
            continue;
        }

        // Add to blend layers
        ozz::animation::BlendingJob::Layer blendLayer;
        blendLayer.transform = ozz::make_span(ozzData->layerLocals[i]);
        blendLayer.weight = layer.weight;

        // Handle blend transition
        if (layer.state.blendDuration > 0.0f) {
            float t = layer.state.blendTime / layer.state.blendDuration;
            blendLayer.weight *= t;
        }

        blendLayers.push_back(blendLayer);
    }

    if (blendLayers.empty()) {
        // No active animations, use bind pose
        const auto& restPose = ozzSkel->joint_rest_poses();
        std::copy(restPose.begin(), restPose.end(), ozzData->locals.begin());
    } else if (blendLayers.size() == 1) {
        // Single layer, no blending needed
        std::copy(ozzData->layerLocals[0].begin(),
                  ozzData->layerLocals[0].end(),
                  ozzData->locals.begin());
    } else {
        // Blend multiple layers
        ozz::animation::BlendingJob blendingJob;
        blendingJob.threshold = 0.1f;
        blendingJob.layers = ozz::make_span(blendLayers);
        blendingJob.rest_pose = ozzSkel->joint_rest_poses();
        blendingJob.output = ozz::make_span(ozzData->locals);

        if (!blendingJob.Run()) {
            fmt::print("Animator::blendLayers: Blending failed\n");
        }
    }
}

void Animator::computeBoneMatrices() {
    const ozz::animation::Skeleton* ozzSkel = skeleton->getOzzSkeleton();
    if (!ozzSkel) return;

    // Convert local transforms to model-space
    ozz::animation::LocalToModelJob ltmJob;
    ltmJob.skeleton = ozzSkel;
    ltmJob.input = ozz::make_span(ozzData->locals);
    ltmJob.output = ozz::make_span(ozzData->models);

    if (!ltmJob.Run()) {
        fmt::print("Animator::computeBoneMatrices: LocalToModel failed\n");
        return;
    }

    // Compute final skinning matrices: inverseBindMatrix * modelMatrix
    const auto& inverseBindMatrices = skeleton->getInverseBindMatrices();
    int numJoints = ozzSkel->num_joints();

    for (int i = 0; i < numJoints; ++i) {
        // Convert ozz Float4x4 to glm::mat4
        const ozz::math::Float4x4& ozzMat = ozzData->models[i];
        glm::mat4 modelMat;

        // ozz stores matrices in column-major order (same as glm)
        for (int col = 0; col < 4; ++col) {
            modelMat[col][0] = ozz::math::GetX(ozzMat.cols[col]);
            modelMat[col][1] = ozz::math::GetY(ozzMat.cols[col]);
            modelMat[col][2] = ozz::math::GetZ(ozzMat.cols[col]);
            modelMat[col][3] = ozz::math::GetW(ozzMat.cols[col]);
        }

        // Final matrix = modelMatrix * inverseBindMatrix
        // (Note: some engines use the opposite order, but this follows glTF convention)
        boneMatrices[i] = modelMat * inverseBindMatrices[i];
    }
}

bool Animator::isPlaying() const {
    for (const auto& layer : layers) {
        if (layer.state.isPlaying) return true;
    }
    return false;
}

const std::string& Animator::getCurrentAnimationName() const {
    static const std::string empty;
    if (!layers.empty() && layers[0].clip) {
        return layers[0].clip->getName();
    }
    return empty;
}

float Animator::getCurrentTime() const {
    if (!layers.empty()) {
        return layers[0].state.currentTime;
    }
    return 0.0f;
}

float Animator::getProgress() const {
    if (!layers.empty() && layers[0].clip) {
        float duration = layers[0].clip->getDuration();
        if (duration > 0.0f) {
            return layers[0].state.currentTime / duration;
        }
    }
    return 0.0f;
}

// ============================================================================
// AnimatorBatch Implementation
// ============================================================================

AnimatorBatch::AnimatorBatch() = default;
AnimatorBatch::~AnimatorBatch() = default;

bool AnimatorBatch::initialize(std::shared_ptr<Skeleton> skel, Uint32 maxInst) {
    if (!skel) return false;

    skeleton = std::move(skel);
    maxInstances = maxInst;

    // Pre-allocate space for packed bone matrices
    Uint32 bonesPerInstance = skeleton->getJointCount();
    packedBoneMatrices.reserve(maxInstances * bonesPerInstance);

    return true;
}

Uint32 AnimatorBatch::addInstance() {
    Uint32 index;

    if (!freeIndices.empty()) {
        index = freeIndices.back();
        freeIndices.pop_back();
    } else {
        if (animators.size() >= maxInstances) {
            return UINT32_MAX;
        }
        index = static_cast<Uint32>(animators.size());
        animators.push_back(nullptr);
    }

    auto animator = std::make_unique<Animator>();
    if (!animator->initialize(skeleton)) {
        freeIndices.push_back(index);
        return UINT32_MAX;
    }

    animators[index] = std::move(animator);
    return index;
}

void AnimatorBatch::removeInstance(Uint32 index) {
    if (index < animators.size() && animators[index]) {
        animators[index].reset();
        freeIndices.push_back(index);
    }
}

Animator* AnimatorBatch::getAnimator(Uint32 index) {
    if (index < animators.size()) {
        return animators[index].get();
    }
    return nullptr;
}

void AnimatorBatch::updateAll(float deltaTime) {
    // TODO: Parallelize with task scheduler for crowd systems
    for (auto& animator : animators) {
        if (animator) {
            animator->update(deltaTime);
        }
    }

    // Pack bone matrices for GPU upload
    Uint32 bonesPerInstance = skeleton->getJointCount();
    packedBoneMatrices.clear();
    packedBoneMatrices.reserve(animators.size() * bonesPerInstance);

    for (const auto& animator : animators) {
        if (animator) {
            const auto& bones = animator->getBoneMatrices();
            packedBoneMatrices.insert(packedBoneMatrices.end(),
                                       bones.begin(), bones.end());
        } else {
            // Insert identity matrices for removed instances
            for (Uint32 i = 0; i < bonesPerInstance; ++i) {
                packedBoneMatrices.push_back(glm::mat4(1.0f));
            }
        }
    }
}

Uint32 AnimatorBatch::getBoneMatrixOffset(Uint32 instanceIndex) const {
    return instanceIndex * skeleton->getJointCount();
}

}  // namespace Vapor
