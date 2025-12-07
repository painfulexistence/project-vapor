#pragma once

#include "Vapor/components.hpp"
#include "action_components.hpp"
#include <entt/entt.hpp>

// ============================================================
// Action System
//
// Unified system for all time-based actions:
// - Single actions (ActionComponent)
// - Queued actions (ActionQueueComponent)
//
// Handles: tweens, waits, callbacks, parallel execution
// ============================================================

class ActionSystem {
public:
    static void update(entt::registry& reg, float dt) {
        updateSingleActions(reg, dt);
        updateActionQueues(reg, dt);
    }

private:
    // ========== Single Actions ==========

    static void updateSingleActions(entt::registry& reg, float dt) {
        auto view = reg.view<ActionComponent>();
        std::vector<entt::entity> completed;

        for (auto entity : view) {
            auto& action = view.get<ActionComponent>(entity);

            if (executeAction(reg, entity, action, dt)) {
                // Check for looping
                if (action.loopMode != LoopMode::None) {
                    if (action.loopCount == -1 || action.currentLoop < action.loopCount - 1) {
                        action.currentLoop++;
                        action.elapsed = 0.0f;
                        action.completed = false;

                        if (action.loopMode == LoopMode::PingPong) {
                            action.pingPongReverse = !action.pingPongReverse;
                        }
                        continue;
                    }
                }
                completed.push_back(entity);
            }
        }

        // Remove completed actions
        for (auto entity : completed) {
            reg.remove<ActionComponent>(entity);
        }
    }

    // ========== Queued Actions ==========

    static void updateActionQueues(entt::registry& reg, float dt) {
        auto view = reg.view<ActionQueueComponent>();
        std::vector<entt::entity> completed;

        for (auto entity : view) {
            auto& queue = view.get<ActionQueueComponent>(entity);

            while (!queue.isComplete()) {
                ActionComponent* current = queue.current();
                if (!current) break;

                bool actionDone = executeAction(reg, entity, *current, dt);

                if (actionDone) {
                    queue.advance();
                    // Don't break - try to execute next instant action in same frame
                } else {
                    break;// Action still running, wait for next frame
                }
            }

            if (queue.isComplete()) {
                if (queue.onComplete) {
                    queue.onComplete();
                }
                completed.push_back(entity);
            }
        }

        // Remove completed queues
        for (auto entity : completed) {
            reg.remove<ActionQueueComponent>(entity);
        }
    }

    // ========== Execute Single Action ==========

    static bool executeAction(entt::registry& reg, entt::entity owner, ActionComponent& action, float dt) {
        // Resolve target
        entt::entity target = (action.target != entt::null) ? action.target : owner;

        // First frame initialization
        if (!action.started) {
            action.started = true;
            initializeAction(reg, target, action);
        }

        // Handle instant actions
        if (action.isInstant()) {
            applyInstantAction(reg, target, action);
            action.completed = true;
            return true;
        }

        // Update elapsed time
        action.elapsed += dt;

        // Apply interpolated value
        applyTweenAction(reg, target, action);

        // Check completion
        if (action.elapsed >= action.duration) {
            action.completed = true;
            return true;
        }

        return false;
    }

    // ========== Initialize Action (capture start values) ==========

    static void initializeAction(entt::registry& reg, entt::entity target, ActionComponent& action) {
        switch (action.type) {
        case ActionType::Position: {
            if (auto* transform = reg.try_get<Vapor::TransformComponent>(target)) {
                // If relative movement (flagged with name="relative")
                if (action.name == "relative") {
                    action.vec3Start = transform->position;
                    action.vec3End = transform->position + action.vec3End;
                    action.name.clear();
                } else if (action.vec3Start == glm::vec3(0.0f) && !action.started) {
                    action.vec3Start = transform->position;
                }
            }
            break;
        }
        case ActionType::Scale: {
            if (auto* transform = reg.try_get<Vapor::TransformComponent>(target)) {
                if (action.vec3Start == glm::vec3(0.0f) && action.vec3End != glm::vec3(0.0f)) {
                    // bounceIn case: start from 0
                } else if (action.vec3Start == glm::vec3(0.0f)) {
                    action.vec3Start = transform->scale;
                }
            }
            break;
        }
        case ActionType::Rotation: {
            if (auto* transform = reg.try_get<Vapor::TransformComponent>(target)) {
                if (action.quatStart == glm::quat(1, 0, 0, 0)) {
                    action.quatStart = transform->rotation;
                }
            }
            break;
        }
        case ActionType::Color: {
            // Could capture from material/sprite component if available
            break;
        }
        case ActionType::Parallel: {
            // Initialize all children
            for (auto& child : action.children) {
                entt::entity childTarget = (child.target != entt::null) ? child.target : target;
                initializeAction(reg, childTarget, child);
            }
            break;
        }
        default:
            break;
        }
    }

    // ========== Apply Instant Action ==========

    static void applyInstantAction(entt::registry& reg, entt::entity target, ActionComponent& action) {
        switch (action.type) {
        case ActionType::Callback: {
            if (action.callback) {
                action.callback();
            }
            break;
        }
        case ActionType::SetActive: {
            if (action.activeValue) {
                reg.emplace_or_replace<Vapor::Active>(target);
            } else {
                reg.remove<Vapor::Active>(target);
            }
            break;
        }
        case ActionType::PlayAnimation: {
            // Would trigger animation system
            // reg.emplace_or_replace<PlayAnimationRequest>(target, action.name);
            break;
        }
        default:
            break;
        }
    }

    // ========== Apply Tween Action ==========

    static void applyTweenAction(entt::registry& reg, entt::entity target, ActionComponent& action) {
        float t = action.getProgress();

        switch (action.type) {
        case ActionType::Position: {
            if (auto* transform = reg.try_get<Vapor::TransformComponent>(target)) {
                transform->position = glm::mix(action.vec3Start, action.vec3End, t);
                transform->isDirty = true;
            }
            break;
        }
        case ActionType::Scale: {
            if (auto* transform = reg.try_get<Vapor::TransformComponent>(target)) {
                transform->scale = glm::mix(action.vec3Start, action.vec3End, t);
                transform->isDirty = true;
            }
            break;
        }
        case ActionType::Rotation: {
            if (auto* transform = reg.try_get<Vapor::TransformComponent>(target)) {
                transform->rotation = glm::slerp(action.quatStart, action.quatEnd, t);
                transform->isDirty = true;
            }
            break;
        }
        case ActionType::Color: {
            // Would apply to material/sprite component
            // glm::vec4 color = glm::mix(action.vec4Start, action.vec4End, t);
            break;
        }
        case ActionType::Wait: {
            // Just wait, nothing to apply
            break;
        }
        case ActionType::Parallel: {
            // Update all children
            for (auto& child : action.children) {
                if (!child.completed) {
                    entt::entity childTarget = (child.target != entt::null) ? child.target : target;
                    executeAction(reg, childTarget, child, 0.0f);// dt already applied to parent
                    child.elapsed = action.elapsed;// Sync elapsed time
                }
            }
            break;
        }
        default:
            break;
        }
    }
};

// ============================================================
// Helper functions for common operations
// ============================================================

namespace ActionHelpers {

    // Play a sequence of actions on an entity
    inline void playSequence(
        entt::registry& reg,
        entt::entity entity,
        std::vector<ActionComponent> actions,
        std::function<void()> onComplete = nullptr,
        const std::string& debugName = ""
    ) {
        auto& queue = reg.emplace_or_replace<ActionQueueComponent>(entity);
        queue.actions = std::move(actions);
        queue.currentIndex = 0;
        queue.onComplete = std::move(onComplete);
        queue.debugName = debugName;
    }

    // Play a single action on an entity
    inline void play(entt::registry& reg, entt::entity entity, ActionComponent action) {
        auto& comp = reg.emplace_or_replace<ActionComponent>(entity);
        static_cast<ActionComponent&>(comp) = std::move(action);
    }

}// namespace ActionHelpers
