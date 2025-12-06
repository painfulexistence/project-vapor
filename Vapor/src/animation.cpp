#include "animation.hpp"

#include <fmt/core.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/base/maths/soa_transform.h>

namespace Vapor {

// ============================================================================
// Skeleton Implementation
// ============================================================================

Skeleton::~Skeleton() = default;

Skeleton::Skeleton(Skeleton&&) noexcept = default;
Skeleton& Skeleton::operator=(Skeleton&&) noexcept = default;

bool Skeleton::initialize(const std::vector<Joint>& inputJoints) {
    if (inputJoints.empty()) {
        fmt::print("Skeleton::initialize: No joints provided\n");
        return false;
    }

    joints = inputJoints;

    // Build name-to-index map and inverse bind matrices
    jointNameToIndex.clear();
    inverseBindMatrices.resize(joints.size());

    for (size_t i = 0; i < joints.size(); ++i) {
        jointNameToIndex[joints[i].name] = static_cast<int>(i);
        inverseBindMatrices[i] = joints[i].inverseBindMatrix;
    }

    // Build ozz skeleton from raw skeleton
    ozz::animation::offline::RawSkeleton rawSkeleton;

    // Count root joints
    int rootCount = 0;
    for (const auto& joint : joints) {
        if (joint.parentIndex < 0) {
            rootCount++;
        }
    }
    rawSkeleton.roots.resize(rootCount);

    // Helper to decompose mat4 into T/R/S
    auto decompose = [](const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s) {
        t = glm::vec3(m[3]);
        s = glm::vec3(
            glm::length(glm::vec3(m[0])),
            glm::length(glm::vec3(m[1])),
            glm::length(glm::vec3(m[2]))
        );
        glm::mat3 rotMat(
            glm::vec3(m[0]) / s.x,
            glm::vec3(m[1]) / s.y,
            glm::vec3(m[2]) / s.z
        );
        r = glm::quat_cast(rotMat);
    };

    // Build hierarchy recursively
    std::function<void(ozz::animation::offline::RawSkeleton::Joint&, int)> buildJoint;
    buildJoint = [&](ozz::animation::offline::RawSkeleton::Joint& ozzJoint, int jointIndex) {
        const Joint& joint = joints[jointIndex];
        ozzJoint.name = joint.name.c_str();

        glm::vec3 t, s;
        glm::quat r;
        decompose(joint.localBindPose, t, r, s);

        ozzJoint.transform.translation = ozz::math::Float3(t.x, t.y, t.z);
        ozzJoint.transform.rotation = ozz::math::Quaternion(r.x, r.y, r.z, r.w);
        ozzJoint.transform.scale = ozz::math::Float3(s.x, s.y, s.z);

        // Find children
        for (size_t i = 0; i < joints.size(); ++i) {
            if (joints[i].parentIndex == jointIndex) {
                ozzJoint.children.emplace_back();
                buildJoint(ozzJoint.children.back(), static_cast<int>(i));
            }
        }
    };

    // Build root joints
    int rootIndex = 0;
    for (size_t i = 0; i < joints.size(); ++i) {
        if (joints[i].parentIndex < 0) {
            buildJoint(rawSkeleton.roots[rootIndex], static_cast<int>(i));
            rootIndex++;
        }
    }

    // Validate raw skeleton
    if (!rawSkeleton.Validate()) {
        fmt::print("Skeleton::initialize: Invalid raw skeleton\n");
        return false;
    }

    // Build runtime skeleton
    ozz::animation::offline::SkeletonBuilder builder;
    ozzSkeleton = std::make_unique<ozz::animation::Skeleton>();
    if (!builder(rawSkeleton, ozzSkeleton.get())) {
        fmt::print("Skeleton::initialize: Failed to build ozz skeleton\n");
        ozzSkeleton.reset();
        return false;
    }

    fmt::print("Skeleton::initialize: Built skeleton with {} joints\n", joints.size());
    return true;
}

int Skeleton::findJointIndex(const std::string& name) const {
    auto it = jointNameToIndex.find(name);
    if (it != jointNameToIndex.end()) {
        return it->second;
    }
    return -1;
}

// ============================================================================
// AnimationClip Implementation
// ============================================================================

AnimationClip::~AnimationClip() = default;

AnimationClip::AnimationClip(AnimationClip&&) noexcept = default;
AnimationClip& AnimationClip::operator=(AnimationClip&&) noexcept = default;

bool AnimationClip::initialize(const std::string& clipName,
                                const std::vector<Channel>& inputChannels,
                                const Skeleton& skeleton) {
    if (inputChannels.empty()) {
        fmt::print("AnimationClip::initialize: No channels provided\n");
        return false;
    }

    if (!skeleton.getOzzSkeleton()) {
        fmt::print("AnimationClip::initialize: Skeleton has no ozz data\n");
        return false;
    }

    name = clipName;
    channels = inputChannels;

    // Calculate duration from channels
    duration = 0.0f;
    for (const auto& channel : channels) {
        if (!channel.timestamps.empty()) {
            duration = std::max(duration, channel.timestamps.back());
        }
    }

    // Build ozz animation from raw animation
    ozz::animation::offline::RawAnimation rawAnimation;
    rawAnimation.duration = duration;

    // Resize tracks to match skeleton joint count
    Uint32 jointCount = skeleton.getJointCount();
    rawAnimation.tracks.resize(jointCount);

    // Initialize default identity transforms for all joints
    for (Uint32 i = 0; i < jointCount; ++i) {
        rawAnimation.tracks[i].translations.push_back({0.0f, {0.0f, 0.0f, 0.0f}});
        rawAnimation.tracks[i].rotations.push_back({0.0f, {0.0f, 0.0f, 0.0f, 1.0f}});
        rawAnimation.tracks[i].scales.push_back({0.0f, {1.0f, 1.0f, 1.0f}});
    }

    // Fill in actual animation data
    for (const auto& channel : channels) {
        if (channel.targetJoint < 0 || channel.targetJoint >= static_cast<int>(jointCount)) {
            continue;
        }

        auto& track = rawAnimation.tracks[channel.targetJoint];

        switch (channel.path) {
            case Channel::Path::Translation: {
                track.translations.clear();
                for (size_t i = 0; i < channel.timestamps.size(); ++i) {
                    ozz::animation::offline::RawAnimation::TranslationKey key;
                    key.time = channel.timestamps[i];
                    key.value = ozz::math::Float3(
                        channel.values[i * 3 + 0],
                        channel.values[i * 3 + 1],
                        channel.values[i * 3 + 2]
                    );
                    track.translations.push_back(key);
                }
                break;
            }
            case Channel::Path::Rotation: {
                track.rotations.clear();
                for (size_t i = 0; i < channel.timestamps.size(); ++i) {
                    ozz::animation::offline::RawAnimation::RotationKey key;
                    key.time = channel.timestamps[i];
                    key.value = ozz::math::Quaternion(
                        channel.values[i * 4 + 0],  // x
                        channel.values[i * 4 + 1],  // y
                        channel.values[i * 4 + 2],  // z
                        channel.values[i * 4 + 3]   // w
                    );
                    track.rotations.push_back(key);
                }
                break;
            }
            case Channel::Path::Scale: {
                track.scales.clear();
                for (size_t i = 0; i < channel.timestamps.size(); ++i) {
                    ozz::animation::offline::RawAnimation::ScaleKey key;
                    key.time = channel.timestamps[i];
                    key.value = ozz::math::Float3(
                        channel.values[i * 3 + 0],
                        channel.values[i * 3 + 1],
                        channel.values[i * 3 + 2]
                    );
                    track.scales.push_back(key);
                }
                break;
            }
        }
    }

    // Validate raw animation
    if (!rawAnimation.Validate()) {
        fmt::print("AnimationClip::initialize: Invalid raw animation '{}'\n", name);
        return false;
    }

    // Build runtime animation
    ozz::animation::offline::AnimationBuilder builder;
    ozzAnimation = std::make_unique<ozz::animation::Animation>();
    if (!builder(rawAnimation, ozzAnimation.get())) {
        fmt::print("AnimationClip::initialize: Failed to build ozz animation '{}'\n", name);
        ozzAnimation.reset();
        return false;
    }

    fmt::print("AnimationClip::initialize: Built animation '{}' (duration: {:.2f}s, {} channels)\n",
               name, duration, channels.size());
    return true;
}

}  // namespace Vapor
