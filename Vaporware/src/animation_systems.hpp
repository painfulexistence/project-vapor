#pragma once

#include "animation_components.hpp"
#include "components.hpp"
#include "Vapor/components.hpp"
#include "Vapor/scene.hpp"
#include <entt/entt.hpp>
#include <fmt/core.h>

// ============================================================
// Tween System - Updates all tween components
// ============================================================

class TweenSystem {
public:
    static void update(entt::registry& reg, float deltaTime) {
        updateFloatTweens(reg, deltaTime);
        updateVec3Tweens(reg, deltaTime);
        updateQuatTweens(reg, deltaTime);
        updateColorTweens(reg, deltaTime);
        updateTransformTweens(reg, deltaTime);
    }

    // Stop all tweens with a specific tag
    static void stopByTag(entt::registry& reg, const std::string& tag) {
        auto stopTweens = [&tag](auto& view) {
            for (auto entity : view) {
                auto& tween = view.template get<typename std::decay_t<decltype(view)>::value_type>(entity);
                if (tween.base.tag == tag) {
                    tween.base.state = TweenState::Completed;
                }
            }
        };

        reg.view<TweenFloatComponent>().each([&](auto entity, auto& tween) {
            if (tween.base.tag == tag) tween.base.state = TweenState::Completed;
        });
        reg.view<TweenVec3Component>().each([&](auto entity, auto& tween) {
            if (tween.base.tag == tag) tween.base.state = TweenState::Completed;
        });
        reg.view<TweenQuatComponent>().each([&](auto entity, auto& tween) {
            if (tween.base.tag == tag) tween.base.state = TweenState::Completed;
        });
        reg.view<TweenColorComponent>().each([&](auto entity, auto& tween) {
            if (tween.base.tag == tag) tween.base.state = TweenState::Completed;
        });
        reg.view<TweenTransformComponent>().each([&](auto entity, auto& tween) {
            if (tween.base.tag == tag) tween.base.state = TweenState::Completed;
        });
    }

    // Pause all tweens
    static void pauseAll(entt::registry& reg) {
        setStateForAll(reg, TweenState::Running, TweenState::Paused);
    }

    // Resume all tweens
    static void resumeAll(entt::registry& reg) {
        setStateForAll(reg, TweenState::Paused, TweenState::Running);
    }

private:
    static void setStateForAll(entt::registry& reg, TweenState from, TweenState to) {
        reg.view<TweenFloatComponent>().each([&](auto entity, auto& tween) {
            if (tween.base.state == from) tween.base.state = to;
        });
        reg.view<TweenVec3Component>().each([&](auto entity, auto& tween) {
            if (tween.base.state == from) tween.base.state = to;
        });
        reg.view<TweenQuatComponent>().each([&](auto entity, auto& tween) {
            if (tween.base.state == from) tween.base.state = to;
        });
        reg.view<TweenColorComponent>().each([&](auto entity, auto& tween) {
            if (tween.base.state == from) tween.base.state = to;
        });
        reg.view<TweenTransformComponent>().each([&](auto entity, auto& tween) {
            if (tween.base.state == from) tween.base.state = to;
        });
    }

    template<typename T>
    static bool updateTweenBase(T& tween, float deltaTime) {
        auto& base = tween.base;

        // Handle different states
        if (base.state == TweenState::Idle || base.state == TweenState::Completed || base.state == TweenState::Paused) {
            return false;
        }

        // Handle delay
        if (base.delay > 0.0f) {
            base.delay -= deltaTime;
            if (base.delay > 0.0f) return false;
            // Carry over extra time
            deltaTime = -base.delay;
            base.delay = 0.0f;
        }

        // Update elapsed time
        base.elapsed += deltaTime;

        // Check completion
        if (base.elapsed >= base.duration) {
            base.elapsed = base.duration;  // Clamp

            // Handle looping
            switch (base.loopMode) {
                case TweenLoopMode::None:
                    base.state = TweenState::Completed;
                    return true;

                case TweenLoopMode::Loop:
                    if (base.loopCount == -1 || base.currentLoop < base.loopCount - 1) {
                        base.elapsed = 0.0f;
                        base.currentLoop++;
                        return false;
                    }
                    base.state = TweenState::Completed;
                    return true;

                case TweenLoopMode::PingPong:
                    if (base.loopCount == -1 || base.currentLoop < base.loopCount - 1) {
                        base.elapsed = 0.0f;
                        base.reverse = !base.reverse;
                        base.currentLoop++;
                        return false;
                    }
                    base.state = TweenState::Completed;
                    return true;
            }
        }

        return false;
    }

    static void updateFloatTweens(entt::registry& reg, float deltaTime) {
        auto view = reg.view<TweenFloatComponent>();
        for (auto entity : view) {
            auto& tween = view.get<TweenFloatComponent>(entity);
            updateTweenBase(tween, deltaTime);

            // Apply value to target if pointer is set
            if (tween.target && tween.base.state == TweenState::Running) {
                *tween.target = tween.getCurrentValue();
            }
        }
    }

    static void updateVec3Tweens(entt::registry& reg, float deltaTime) {
        auto view = reg.view<TweenVec3Component>();
        for (auto entity : view) {
            auto& tween = view.get<TweenVec3Component>(entity);
            updateTweenBase(tween, deltaTime);
        }
    }

    static void updateQuatTweens(entt::registry& reg, float deltaTime) {
        auto view = reg.view<TweenQuatComponent>();
        for (auto entity : view) {
            auto& tween = view.get<TweenQuatComponent>(entity);
            updateTweenBase(tween, deltaTime);
        }
    }

    static void updateColorTweens(entt::registry& reg, float deltaTime) {
        auto view = reg.view<TweenColorComponent>();
        for (auto entity : view) {
            auto& tween = view.get<TweenColorComponent>(entity);
            updateTweenBase(tween, deltaTime);
        }
    }

    static void updateTransformTweens(entt::registry& reg, float deltaTime) {
        // Update tweens that target TransformComponent
        auto view = reg.view<TweenTransformComponent, Vapor::TransformComponent>();
        for (auto entity : view) {
            auto& tween = view.get<TweenTransformComponent>(entity);
            auto& transform = view.get<Vapor::TransformComponent>(entity);

            if (tween.base.state != TweenState::Running) continue;

            updateTweenBase(tween, deltaTime);
            float progress = tween.base.getEasedProgress();

            switch (tween.target) {
                case TweenTransformTarget::Position:
                    transform.position = glm::mix(tween.startPosition, tween.endPosition, progress);
                    transform.isDirty = true;
                    break;
                case TweenTransformTarget::Rotation:
                    transform.rotation = glm::slerp(tween.startRotation, tween.endRotation, progress);
                    transform.isDirty = true;
                    break;
                case TweenTransformTarget::Scale:
                    transform.scale = glm::mix(tween.startScale, tween.endScale, progress);
                    transform.isDirty = true;
                    break;
            }
        }

        // Also update tweens on SceneNode references
        auto nodeView = reg.view<TweenTransformComponent, SceneNodeReferenceComponent>();
        for (auto entity : nodeView) {
            auto& tween = nodeView.get<TweenTransformComponent>(entity);
            auto& nodeRef = nodeView.get<SceneNodeReferenceComponent>(entity);

            if (tween.base.state != TweenState::Running || !nodeRef.node) continue;

            updateTweenBase(tween, deltaTime);
            float progress = tween.base.getEasedProgress();

            switch (tween.target) {
                case TweenTransformTarget::Position:
                    nodeRef.node->setLocalPosition(glm::mix(tween.startPosition, tween.endPosition, progress));
                    break;
                case TweenTransformTarget::Rotation:
                    nodeRef.node->setLocalRotation(glm::slerp(tween.startRotation, tween.endRotation, progress));
                    break;
                case TweenTransformTarget::Scale:
                    nodeRef.node->setLocalScale(glm::mix(tween.startScale, tween.endScale, progress));
                    break;
            }
        }
    }
};

// ============================================================
// Sprite Animation System - Updates sprite animations
// ============================================================

class SpriteAnimationSystem {
public:
    static void update(entt::registry& reg, float deltaTime) {
        updateSimpleAnimations(reg, deltaTime);
        updateAnimators(reg, deltaTime);
    }

    // Play a specific animation on an entity
    static void play(entt::registry& reg, entt::entity entity, const std::string& clipName, bool restart = false) {
        if (auto* animator = reg.try_get<SpriteAnimatorComponent>(entity)) {
            animator->play(clipName, restart);
        }
    }

    // Stop animation on an entity
    static void stop(entt::registry& reg, entt::entity entity) {
        if (auto* anim = reg.try_get<SpriteAnimationComponent>(entity)) {
            anim->isPlaying = false;
        }
        if (auto* animator = reg.try_get<SpriteAnimatorComponent>(entity)) {
            animator->isPlaying = false;
        }
    }

    // Pause all sprite animations
    static void pauseAll(entt::registry& reg) {
        reg.view<SpriteAnimationComponent>().each([](auto& anim) { anim.isPlaying = false; });
        reg.view<SpriteAnimatorComponent>().each([](auto& anim) { anim.isPlaying = false; });
    }

private:
    static void updateSimpleAnimations(entt::registry& reg, float deltaTime) {
        auto view = reg.view<SpriteAnimationComponent>();
        for (auto entity : view) {
            auto& anim = view.get<SpriteAnimationComponent>(entity);

            if (!anim.isPlaying || anim.frames.empty()) continue;

            // Update frame timer
            anim.frameTimer += deltaTime * anim.playbackSpeed;

            const auto& currentFrame = anim.frames[anim.currentFrameIndex];
            if (anim.frameTimer >= currentFrame.duration) {
                anim.frameTimer -= currentFrame.duration;

                int prevFrame = anim.currentFrameIndex;
                int nextFrame = anim.currentFrameIndex;

                // Advance frame
                if (anim.reverse) {
                    nextFrame--;
                } else {
                    nextFrame++;
                }

                // Handle frame bounds
                bool reachedEnd = false;
                if (nextFrame < 0) {
                    nextFrame = 0;
                    reachedEnd = true;
                } else if (nextFrame >= static_cast<int>(anim.frames.size())) {
                    nextFrame = static_cast<int>(anim.frames.size()) - 1;
                    reachedEnd = true;
                }

                if (reachedEnd) {
                    switch (anim.playMode) {
                        case AnimationPlayMode::Once:
                            anim.isPlaying = false;
                            if (anim.onComplete) anim.onComplete();
                            break;
                        case AnimationPlayMode::Loop:
                            nextFrame = anim.reverse ? static_cast<int>(anim.frames.size()) - 1 : 0;
                            break;
                        case AnimationPlayMode::PingPong:
                            anim.reverse = !anim.reverse;
                            nextFrame = anim.currentFrameIndex + (anim.reverse ? -1 : 1);
                            nextFrame = std::clamp(nextFrame, 0, static_cast<int>(anim.frames.size()) - 1);
                            break;
                        case AnimationPlayMode::ClampForever:
                            // Stay on last frame
                            break;
                    }
                }

                anim.currentFrameIndex = nextFrame;

                if (prevFrame != anim.currentFrameIndex && anim.onFrameChange) {
                    anim.onFrameChange(anim.currentFrameIndex);
                }
            }
        }
    }

    static void updateAnimators(entt::registry& reg, float deltaTime) {
        auto view = reg.view<SpriteAnimatorComponent>();
        for (auto entity : view) {
            auto& animator = view.get<SpriteAnimatorComponent>(entity);

            if (!animator.isPlaying) continue;

            const auto* clip = animator.getCurrentClip();
            if (!clip || clip->frames.empty()) continue;

            // Update frame timer
            animator.frameTimer += deltaTime * animator.playbackSpeed;

            const auto& currentFrame = clip->frames[animator.currentFrameIndex];
            if (animator.frameTimer >= currentFrame.duration) {
                animator.frameTimer -= currentFrame.duration;

                int prevFrame = animator.currentFrameIndex;
                int nextFrame = animator.currentFrameIndex;

                // Advance frame
                if (animator.reverse) {
                    nextFrame--;
                } else {
                    nextFrame++;
                }

                // Handle frame bounds
                bool reachedEnd = false;
                if (nextFrame < 0) {
                    nextFrame = 0;
                    reachedEnd = true;
                } else if (nextFrame >= static_cast<int>(clip->frames.size())) {
                    nextFrame = static_cast<int>(clip->frames.size()) - 1;
                    reachedEnd = true;
                }

                if (reachedEnd) {
                    switch (clip->defaultPlayMode) {
                        case AnimationPlayMode::Once:
                            animator.isPlaying = false;
                            if (animator.onClipComplete) animator.onClipComplete(animator.currentClipName);
                            break;
                        case AnimationPlayMode::Loop:
                            nextFrame = animator.reverse ? static_cast<int>(clip->frames.size()) - 1 : 0;
                            break;
                        case AnimationPlayMode::PingPong:
                            animator.reverse = !animator.reverse;
                            nextFrame = animator.currentFrameIndex + (animator.reverse ? -1 : 1);
                            nextFrame = std::clamp(nextFrame, 0, static_cast<int>(clip->frames.size()) - 1);
                            break;
                        case AnimationPlayMode::ClampForever:
                            break;
                    }
                }

                animator.currentFrameIndex = nextFrame;

                if (prevFrame != animator.currentFrameIndex && animator.onFrameChange) {
                    animator.onFrameChange(animator.currentFrameIndex);
                }
            }
        }
    }
};

// ============================================================
// Timeline / Cutscene System - Updates timelines and cutscenes
// ============================================================

class TimelineSystem {
public:
    static void update(entt::registry& reg, float deltaTime) {
        updateTimelines(reg, deltaTime);
        updateCutscenes(reg, deltaTime);
        cleanupCompleted(reg);
    }

    // Start a timeline by tag
    static void playByTag(entt::registry& reg, const std::string& tag) {
        reg.view<TimelineComponent>().each([&](auto& timeline) {
            if (timeline.tag == tag) timeline.play();
        });
        reg.view<ActionQueueComponent>().each([&](auto& cutscene) {
            if (cutscene.tag == tag) cutscene.play();
        });
    }

    // Stop a timeline by tag
    static void stopByTag(entt::registry& reg, const std::string& tag) {
        reg.view<TimelineComponent>().each([&](auto& timeline) {
            if (timeline.tag == tag) timeline.stop();
        });
        reg.view<ActionQueueComponent>().each([&](auto& cutscene) {
            if (cutscene.tag == tag) cutscene.stop();
        });
    }

    // Pause all timelines
    static void pauseAll(entt::registry& reg) {
        reg.view<TimelineComponent>().each([](auto& timeline) { timeline.pause(); });
        reg.view<ActionQueueComponent>().each([](auto& cutscene) { cutscene.pause(); });
    }

    // Resume all timelines
    static void resumeAll(entt::registry& reg) {
        reg.view<TimelineComponent>().each([](auto& timeline) { timeline.resume(); });
        reg.view<ActionQueueComponent>().each([](auto& cutscene) {
            if (cutscene.state == TimelineState::Paused) cutscene.state = TimelineState::Playing;
        });
    }

private:
    static void updateTimelines(entt::registry& reg, float deltaTime) {
        auto view = reg.view<TimelineComponent>();
        for (auto entity : view) {
            auto& timeline = view.get<TimelineComponent>(entity);

            if (timeline.state != TimelineState::Playing) continue;

            // Fire onStart callback
            if (timeline.currentTime == 0.0f && timeline.onStart) {
                timeline.onStart();
            }

            // Update time
            timeline.currentTime += deltaTime * timeline.playbackSpeed;

            // Update all tracks
            bool allTracksComplete = true;
            for (auto& track : timeline.tracks) {
                if (!track.isComplete) {
                    updateTrack(reg, track, deltaTime * timeline.playbackSpeed);
                    if (!track.isComplete) allTracksComplete = false;
                }
            }

            // Fire onUpdate callback
            if (timeline.onUpdate) {
                timeline.onUpdate(timeline.getProgress());
            }

            // Check completion
            if (allTracksComplete) {
                timeline.state = TimelineState::Completed;
                if (timeline.onComplete) timeline.onComplete();
            }
        }
    }

    static void updateTrack(entt::registry& reg, TimelineTrack& track, float deltaTime) {
        if (track.isComplete) return;

        TimelineAction* action = track.getCurrentAction();
        if (!action) {
            track.isComplete = true;
            return;
        }

        // Start action if not started
        if (!action->started) {
            startAction(reg, *action);
            action->started = true;
        }

        // Update action
        bool actionComplete = updateAction(reg, *action, deltaTime);

        if (actionComplete) {
            action->completed = true;
            track.currentActionIndex++;
            if (track.currentActionIndex >= track.actions.size()) {
                track.isComplete = true;
            }
        }
    }

    static void updateCutscenes(entt::registry& reg, float deltaTime) {
        auto view = reg.view<ActionQueueComponent>();
        for (auto entity : view) {
            auto& cutscene = view.get<ActionQueueComponent>(entity);

            if (cutscene.state != TimelineState::Playing) continue;

            // Fire onStart callback (only once)
            if (cutscene.currentActionIndex == 0 && cutscene.actions.size() > 0) {
                auto& firstAction = cutscene.actions[0];
                if (!firstAction.started && cutscene.onStart) {
                    cutscene.onStart();
                }
            }

            TimelineAction* action = cutscene.getCurrentAction();
            if (!action) {
                cutscene.state = TimelineState::Completed;
                if (cutscene.onComplete) cutscene.onComplete();
                continue;
            }

            // Start action if not started
            if (!action->started) {
                startAction(reg, *action);
                action->started = true;
            }

            // Update action
            bool actionComplete = updateAction(reg, *action, deltaTime * cutscene.playbackSpeed);

            if (actionComplete) {
                action->completed = true;
                cutscene.currentActionIndex++;

                if (cutscene.isComplete()) {
                    cutscene.state = TimelineState::Completed;
                    if (cutscene.onComplete) cutscene.onComplete();
                }
            }
        }
    }

    static void startAction(entt::registry& reg, TimelineAction& action) {
        switch (action.type) {
            case TimelineActionType::MoveTo:
                // Capture start position
                if (reg.valid(action.targetEntity)) {
                    if (auto* transform = reg.try_get<Vapor::TransformComponent>(action.targetEntity)) {
                        action.vec3Start = transform->position;
                    } else if (auto* nodeRef = reg.try_get<SceneNodeReferenceComponent>(action.targetEntity)) {
                        if (nodeRef->node) {
                            action.vec3Start = nodeRef->node->getLocalPosition();
                        }
                    }
                }
                break;

            case TimelineActionType::RotateTo:
                // Capture start rotation
                if (reg.valid(action.targetEntity)) {
                    if (auto* transform = reg.try_get<Vapor::TransformComponent>(action.targetEntity)) {
                        action.quatStart = transform->rotation;
                    } else if (auto* nodeRef = reg.try_get<SceneNodeReferenceComponent>(action.targetEntity)) {
                        if (nodeRef->node) {
                            action.quatStart = nodeRef->node->getLocalRotation();
                        }
                    }
                }
                break;

            case TimelineActionType::ScaleTo:
                // Capture start scale
                if (reg.valid(action.targetEntity)) {
                    if (auto* transform = reg.try_get<Vapor::TransformComponent>(action.targetEntity)) {
                        action.vec3Start = transform->scale;
                    } else if (auto* nodeRef = reg.try_get<SceneNodeReferenceComponent>(action.targetEntity)) {
                        if (nodeRef->node) {
                            action.vec3Start = nodeRef->node->getLocalScale();
                        }
                    }
                }
                break;

            case TimelineActionType::Callback:
                if (action.callback) action.callback();
                break;

            case TimelineActionType::PlayAnimation:
                if (reg.valid(action.targetEntity)) {
                    SpriteAnimationSystem::play(reg, action.targetEntity, action.stringData, true);
                }
                break;

            case TimelineActionType::Parallel:
                // Start all child actions
                for (auto& child : action.children) {
                    startAction(reg, child);
                    child.started = true;
                }
                break;

            case TimelineActionType::Sequence:
                // Start first child action
                if (!action.children.empty()) {
                    startAction(reg, action.children[0]);
                    action.children[0].started = true;
                }
                break;

            default:
                break;
        }
    }

    static bool updateAction(entt::registry& reg, TimelineAction& action, float deltaTime) {
        if (action.completed) return true;

        action.elapsed += deltaTime;
        float progress = action.duration > 0.0f ? std::min(action.elapsed / action.duration, 1.0f) : 1.0f;
        float easedProgress = action.easing ? action.easing(progress) : progress;

        switch (action.type) {
            case TimelineActionType::Wait:
                return action.elapsed >= action.duration;

            case TimelineActionType::MoveTo:
                if (reg.valid(action.targetEntity)) {
                    glm::vec3 newPos = glm::mix(action.vec3Start, action.vec3End, easedProgress);
                    if (auto* transform = reg.try_get<Vapor::TransformComponent>(action.targetEntity)) {
                        transform->position = newPos;
                        transform->isDirty = true;
                    } else if (auto* nodeRef = reg.try_get<SceneNodeReferenceComponent>(action.targetEntity)) {
                        if (nodeRef->node) nodeRef->node->setLocalPosition(newPos);
                    }
                }
                return action.elapsed >= action.duration;

            case TimelineActionType::RotateTo:
                if (reg.valid(action.targetEntity)) {
                    glm::quat newRot = glm::slerp(action.quatStart, action.quatEnd, easedProgress);
                    if (auto* transform = reg.try_get<Vapor::TransformComponent>(action.targetEntity)) {
                        transform->rotation = newRot;
                        transform->isDirty = true;
                    } else if (auto* nodeRef = reg.try_get<SceneNodeReferenceComponent>(action.targetEntity)) {
                        if (nodeRef->node) nodeRef->node->setLocalRotation(newRot);
                    }
                }
                return action.elapsed >= action.duration;

            case TimelineActionType::ScaleTo:
                if (reg.valid(action.targetEntity)) {
                    glm::vec3 newScale = glm::mix(action.vec3Start, action.vec3End, easedProgress);
                    if (auto* transform = reg.try_get<Vapor::TransformComponent>(action.targetEntity)) {
                        transform->scale = newScale;
                        transform->isDirty = true;
                    } else if (auto* nodeRef = reg.try_get<SceneNodeReferenceComponent>(action.targetEntity)) {
                        if (nodeRef->node) nodeRef->node->setLocalScale(newScale);
                    }
                }
                return action.elapsed >= action.duration;

            case TimelineActionType::FadeIn:
            case TimelineActionType::FadeOut:
                // These would update a material/sprite opacity component
                // Implementation depends on rendering system
                return action.elapsed >= action.duration;

            case TimelineActionType::SetActive:
                // Implementation depends on how "active" is represented
                // Could add/remove an Active tag component
                if (reg.valid(action.targetEntity)) {
                    if (action.boolData) {
                        reg.emplace_or_replace<Vapor::Active>(action.targetEntity);
                    } else {
                        reg.remove<Vapor::Active>(action.targetEntity);
                    }
                }
                return true;  // Instant action

            case TimelineActionType::Callback:
                return true;  // Already executed in startAction

            case TimelineActionType::PlayAnimation:
                return true;  // Instant action (animation plays on its own)

            case TimelineActionType::CameraLookAt:
                if (reg.valid(action.targetEntity)) {
                    if (auto* cam = reg.try_get<Vapor::VirtualCameraComponent>(action.targetEntity)) {
                        glm::vec3 direction = glm::normalize(action.vec3End - cam->position);
                        glm::quat targetRot = glm::quatLookAt(direction, glm::vec3(0, 1, 0));
                        cam->rotation = glm::slerp(action.quatStart, targetRot, easedProgress);
                    }
                }
                return action.elapsed >= action.duration;

            case TimelineActionType::CameraMoveTo:
                if (reg.valid(action.targetEntity)) {
                    if (auto* cam = reg.try_get<Vapor::VirtualCameraComponent>(action.targetEntity)) {
                        cam->position = glm::mix(action.vec3Start, action.vec3End, easedProgress);
                    }
                }
                return action.elapsed >= action.duration;

            case TimelineActionType::Parallel: {
                bool allComplete = true;
                for (auto& child : action.children) {
                    if (!child.completed) {
                        bool childComplete = updateAction(reg, child, deltaTime);
                        if (childComplete) child.completed = true;
                        else allComplete = false;
                    }
                }
                return allComplete;
            }

            case TimelineActionType::Sequence: {
                // Find current incomplete child action
                for (size_t i = 0; i < action.children.size(); ++i) {
                    auto& child = action.children[i];
                    if (!child.completed) {
                        if (!child.started) {
                            startAction(reg, child);
                            child.started = true;
                        }
                        bool childComplete = updateAction(reg, child, deltaTime);
                        if (childComplete) {
                            child.completed = true;
                        }
                        return false;  // Still processing this child
                    }
                }
                return true;  // All children complete
            }

            default:
                return true;
        }
    }

    static void cleanupCompleted(entt::registry& reg) {
        // Remove completed timelines with autoDestroy flag
        std::vector<entt::entity> toRemove;

        reg.view<TimelineComponent>().each([&](auto entity, auto& timeline) {
            if (timeline.state == TimelineState::Completed && timeline.autoDestroy) {
                toRemove.push_back(entity);
            }
        });

        reg.view<ActionQueueComponent>().each([&](auto entity, auto& cutscene) {
            if (cutscene.state == TimelineState::Completed && cutscene.autoDestroy) {
                toRemove.push_back(entity);
            }
        });

        for (auto entity : toRemove) {
            reg.remove<TimelineComponent>(entity);
            reg.remove<ActionQueueComponent>(entity);
        }
    }
};

// ============================================================
// Unified Animation System - Updates all animation components
// ============================================================

class AnimationSystem {
public:
    static void update(entt::registry& reg, float deltaTime) {
        TweenSystem::update(reg, deltaTime);
        SpriteAnimationSystem::update(reg, deltaTime);
        TimelineSystem::update(reg, deltaTime);
    }

    static void pauseAll(entt::registry& reg) {
        TweenSystem::pauseAll(reg);
        SpriteAnimationSystem::pauseAll(reg);
        TimelineSystem::pauseAll(reg);
    }

    static void resumeAll(entt::registry& reg) {
        TweenSystem::resumeAll(reg);
        TimelineSystem::resumeAll(reg);
    }
};

// ============================================================
// Helper Functions for Common Use Cases
// ============================================================

namespace AnimationHelpers {

    // Start a simple position tween on an entity
    inline void tweenPosition(
        entt::registry& reg,
        entt::entity entity,
        const glm::vec3& from,
        const glm::vec3& to,
        float duration,
        EasingFunction easing = Easing::OutCubic,
        const std::string& tag = ""
    ) {
        auto& tween = reg.emplace_or_replace<TweenTransformComponent>(entity);
        tween.base.duration = duration;
        tween.base.easing = easing;
        tween.base.state = TweenState::Running;
        tween.base.tag = tag;
        tween.target = TweenTransformTarget::Position;
        tween.startPosition = from;
        tween.endPosition = to;
    }

    // Start a simple rotation tween on an entity
    inline void tweenRotation(
        entt::registry& reg,
        entt::entity entity,
        const glm::quat& from,
        const glm::quat& to,
        float duration,
        EasingFunction easing = Easing::OutCubic,
        const std::string& tag = ""
    ) {
        auto& tween = reg.emplace_or_replace<TweenTransformComponent>(entity);
        tween.base.duration = duration;
        tween.base.easing = easing;
        tween.base.state = TweenState::Running;
        tween.base.tag = tag;
        tween.target = TweenTransformTarget::Rotation;
        tween.startRotation = from;
        tween.endRotation = to;
    }

    // Start a simple scale tween on an entity
    inline void tweenScale(
        entt::registry& reg,
        entt::entity entity,
        const glm::vec3& from,
        const glm::vec3& to,
        float duration,
        EasingFunction easing = Easing::OutCubic,
        const std::string& tag = ""
    ) {
        auto& tween = reg.emplace_or_replace<TweenTransformComponent>(entity);
        tween.base.duration = duration;
        tween.base.easing = easing;
        tween.base.state = TweenState::Running;
        tween.base.tag = tag;
        tween.target = TweenTransformTarget::Scale;
        tween.startScale = from;
        tween.endScale = to;
    }

    // Create and play a simple cutscene
    inline void playCutscene(
        entt::registry& reg,
        entt::entity entity,
        std::vector<TimelineAction> actions,
        std::function<void()> onComplete = nullptr,
        const std::string& tag = ""
    ) {
        auto& cutscene = reg.emplace_or_replace<ActionQueueComponent>(entity);
        cutscene.actions = std::move(actions);
        cutscene.tag = tag;
        cutscene.onComplete = std::move(onComplete);
        cutscene.autoDestroy = true;
        cutscene.play();
    }

} // namespace AnimationHelpers
