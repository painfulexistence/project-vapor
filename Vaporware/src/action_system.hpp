#pragma once

#include "Vapor/components.hpp"
#include "action_components.hpp"
#include <entt/entt.hpp>
#include <fmt/core.h>

// Action event tags
constexpr uint32_t DOOR_OPENED = 1;

// ============================================================
// Action System
//
// Executes actions attached to entities:
// - ActionComponent: single action with runtime state
// - ActionQueueComponent: sequential actions
// - ActionGroupComponent: parallel action tracking
// ============================================================

class ActionSystem {
public:
    static void update(entt::registry& reg, float dt) {
        updateSingleActions(reg, dt);
        updateActionQueues(reg, dt);
        updateActionGroups(reg);
        auto view = reg.view<ActionCompleteEvent>();
        for (auto entity : view) {
            auto& event = view.get<ActionCompleteEvent>(entity);
            if (event.tag == DOOR_OPENED) {
                // React: play sound, spawn particles, etc.
                fmt::print("Door opened!\n");
                reg.destroy(entity);
            }
        }
    }

private:
    // ========== Single Actions ==========

    static void updateSingleActions(entt::registry& reg, float dt) {
        auto view = reg.view<ActionComponent>();
        std::vector<entt::entity> completed;

        for (auto entity : view) {
            auto& comp = view.get<ActionComponent>(entity);

            if (executeAction(reg, entity, comp.action, comp, dt)) {
                // Check for looping
                if (comp.action.loopMode != LoopMode::None) {
                    if (comp.action.loopCount == -1 || comp.currentLoop < comp.action.loopCount - 1) {
                        comp.currentLoop++;
                        comp.elapsed = 0.0f;
                        comp.completed = false;

                        if (comp.action.loopMode == LoopMode::PingPong) {
                            comp.pingPongReverse = !comp.pingPongReverse;
                        }
                        continue;
                    }
                }
                completed.push_back(entity);
            }
        }

        // Handle completed actions
        for (auto entity : completed) {
            auto& comp = reg.get<ActionComponent>(entity);

            if (comp.action.completionTag != 0) {
                emitCompleteEvent(reg, comp.action.completionTag);
            }

            if (auto* member = reg.try_get<ActionGroupMemberTag>(entity)) {
                if (reg.valid(member->groupEntity)) {
                    if (auto* group = reg.try_get<ActionGroupComponent>(member->groupEntity)) {
                        group->completedActions++;
                    }
                }
            }

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
                Action* current = queue.current();
                if (!current) break;

                bool actionDone = executeQueuedAction(reg, entity, *current, queue, dt);

                if (actionDone) {
                    if (current->completionTag != 0) {
                        emitCompleteEvent(reg, current->completionTag);
                    }
                    queue.advance();
                } else {
                    break;
                }
            }

            if (queue.isComplete()) {
                if (queue.completionTag != 0) {
                    emitCompleteEvent(reg, queue.completionTag);
                }
                completed.push_back(entity);
            }
        }

        for (auto entity : completed) {
            reg.remove<ActionQueueComponent>(entity);
        }
    }

    // ========== Action Groups ==========

    static void updateActionGroups(entt::registry& reg) {
        auto view = reg.view<ActionGroupComponent>();
        std::vector<entt::entity> completed;

        for (auto entity : view) {
            auto& group = view.get<ActionGroupComponent>(entity);

            if (group.isComplete()) {
                if (group.completionTag != 0) {
                    emitCompleteEvent(reg, group.completionTag);
                }
                completed.push_back(entity);
            }
        }

        for (auto entity : completed) {
            reg.destroy(entity);
        }
    }

    // ========== Emit Completion Event ==========

    static void emitCompleteEvent(entt::registry& reg, uint32_t tag) {
        auto eventEntity = reg.create();
        reg.emplace<ActionCompleteEvent>(eventEntity, tag);
    }

    // ========== Execute Single Action (ActionComponent) ==========

    static bool
        executeAction(entt::registry& reg, entt::entity owner, Action& action, ActionComponent& state, float dt) {
        entt::entity target = (action.target != entt::null) ? action.target : owner;

        if (!state.started) {
            state.started = true;
            initializeAction(reg, target, action);
        }

        if (action.isInstant()) {
            applyInstantAction(reg, target, action);
            state.completed = true;
            return true;
        }

        state.elapsed += dt;
        applyTweenAction(reg, target, action, state);

        if (state.elapsed >= action.duration) {
            state.completed = true;
            return true;
        }

        return false;
    }

    // ========== Execute Queued Action (uses queue's runtime state) ==========

    static bool executeQueuedAction(
        entt::registry& reg, entt::entity owner, Action& action, ActionQueueComponent& queue, float dt
    ) {
        entt::entity target = (action.target != entt::null) ? action.target : owner;

        if (!queue.started) {
            queue.started = true;
            initializeAction(reg, target, action);
        }

        if (action.isInstant()) {
            applyInstantAction(reg, target, action);
            return true;
        }

        queue.elapsed += dt;
        applyTweenActionFromQueue(reg, target, action, queue);

        if (queue.elapsed >= action.duration) {
            return true;
        }

        return false;
    }

    // ========== Initialize Action ==========

    static void initializeAction(entt::registry& reg, entt::entity target, Action& action) {
        switch (action.type) {
        case ActionType::Position: {
            if (auto* transform = reg.try_get<Vapor::TransformComponent>(target)) {
                if (action.name == "relative") {
                    action.vec3Start = transform->position;
                    action.vec3End = transform->position + action.vec3End;
                    action.name.clear();
                } else if (action.vec3Start == glm::vec3(0.0f)) {
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
        default:
            break;
        }
    }

    // ========== Apply Instant Action ==========

    static void applyInstantAction(entt::registry& reg, entt::entity target, Action& action) {
        switch (action.type) {
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
            break;
        }
        default:
            break;
        }
    }

    // ========== Apply Tween (ActionComponent) ==========

    static void applyTweenAction(entt::registry& reg, entt::entity target, Action& action, ActionComponent& state) {
        float t = state.getProgress();
        applyTweenValue(reg, target, action, t);
    }

    // ========== Apply Tween (Queue) ==========

    static void applyTweenActionFromQueue(
        entt::registry& reg, entt::entity target, Action& action, ActionQueueComponent& queue
    ) {
        float rawT = (action.duration > 0.0f) ? std::clamp(queue.elapsed / action.duration, 0.0f, 1.0f) : 1.0f;
        float t = action.easing ? action.easing(rawT) : rawT;
        applyTweenValue(reg, target, action, t);
    }

    // ========== Apply Tween Value ==========

    static void applyTweenValue(entt::registry& reg, entt::entity target, Action& action, float t) {
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
            break;
        }
        case ActionType::Wait: {
            break;
        }
        default:
            break;
        }
    }
};

// ============================================================
// Event Cleanup System
// ============================================================

class ActionEventCleanupSystem {
public:
    static void update(entt::registry& reg) {
        reg.clear<ActionCompleteEvent>();
    }
};
