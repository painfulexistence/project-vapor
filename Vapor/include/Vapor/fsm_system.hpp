#pragma once

#include "fsm.hpp"
#include <entt/entt.hpp>

namespace Vapor {

// ============================================================
// FSM System - Pure Data Processing
// ============================================================

/**
 * Initialize FSM for an entity.
 * Call once when setting up an entity with FSM components.
 */
inline void initFSM(FSMStateComponent& state, const FSMDefinition& def) {
    state.currentState = def.initialState;
    state.previousState = def.initialState;
    state.stateTime = 0.0f;
    state.totalTime = 0.0f;
    state.justEntered = true;
    state.justExited = false;
}

/**
 * Force transition to a specific state.
 */
inline void transitionFSM(FSMStateComponent& state, uint32_t newState) {
    if (state.currentState != newState) {
        state.previousState = state.currentState;
        state.currentState = newState;
        state.stateTime = 0.0f;
        state.justEntered = true;
        state.justExited = true;
    }
}

/**
 * Main FSM update system.
 *
 * Processes:
 * 1. Resets per-frame flags
 * 2. Processes event queue
 * 3. Checks timed transitions
 * 4. Updates state time
 */
inline void updateFSMSystem(entt::registry& registry, float deltaTime) {
    auto view = registry.view<FSMStateComponent, FSMDefinition>();

    for (auto entity : view) {
        auto& state = view.get<FSMStateComponent>(entity);
        auto& def = view.get<FSMDefinition>(entity);

        // 1. Reset per-frame flags
        state.justEntered = false;
        state.justExited = false;

        // 2. Process events if entity has event queue
        if (auto* events = registry.try_get<FSMEventQueue>(entity)) {
            for (const auto& event : events->events) {
                // Find matching transition
                for (const auto& transition : def.eventTransitions) {
                    if (transition.fromState == state.currentState &&
                        transition.triggerEvent == event &&
                        state.stateTime >= transition.minStateTime) {
                        // Execute transition
                        state.previousState = state.currentState;
                        state.currentState = transition.toState;
                        state.stateTime = 0.0f;
                        state.justEntered = true;
                        state.justExited = true;
                        break;
                    }
                }
            }
            events->clear();
        }

        // 3. Check timed transitions
        for (const auto& timed : def.timedTransitions) {
            if (timed.fromState == state.currentState &&
                state.stateTime >= timed.duration) {
                state.previousState = state.currentState;
                state.currentState = timed.toState;
                state.stateTime = 0.0f;
                state.justEntered = true;
                state.justExited = true;
                break;
            }
        }

        // 4. Update timers
        state.stateTime += deltaTime;
        state.totalTime += deltaTime;
    }
}

// ============================================================
// Helper Functions
// ============================================================

/**
 * Send an event to a specific entity's FSM.
 */
inline void sendFSMEvent(entt::registry& registry, entt::entity entity, const std::string& event) {
    if (auto* events = registry.try_get<FSMEventQueue>(entity)) {
        events->push(event);
    }
}

/**
 * Broadcast an event to all FSMs.
 */
inline void broadcastFSMEvent(entt::registry& registry, const std::string& event) {
    auto view = registry.view<FSMEventQueue>();
    for (auto entity : view) {
        auto& events = view.get<FSMEventQueue>(entity);
        events.push(event);
    }
}

/**
 * Check if entity is in a specific state (by name).
 */
inline bool isInFSMState(entt::registry& registry, entt::entity entity, const std::string& stateName) {
    auto* state = registry.try_get<FSMStateComponent>(entity);
    auto* def = registry.try_get<FSMDefinition>(entity);
    if (state && def) {
        return state->currentState == def->getStateIndex(stateName);
    }
    return false;
}

/**
 * Check if entity just entered a specific state this frame.
 */
inline bool justEnteredFSMState(entt::registry& registry, entt::entity entity, const std::string& stateName) {
    auto* state = registry.try_get<FSMStateComponent>(entity);
    auto* def = registry.try_get<FSMDefinition>(entity);
    if (state && def && state->justEntered) {
        return state->currentState == def->getStateIndex(stateName);
    }
    return false;
}

/**
 * Check if entity just exited a specific state this frame.
 */
inline bool justExitedFSMState(entt::registry& registry, entt::entity entity, const std::string& stateName) {
    auto* state = registry.try_get<FSMStateComponent>(entity);
    auto* def = registry.try_get<FSMDefinition>(entity);
    if (state && def && state->justExited) {
        return state->previousState == def->getStateIndex(stateName);
    }
    return false;
}

/**
 * Get current state name.
 */
inline const std::string& getFSMStateName(entt::registry& registry, entt::entity entity) {
    static const std::string empty;
    auto* state = registry.try_get<FSMStateComponent>(entity);
    auto* def = registry.try_get<FSMDefinition>(entity);
    if (state && def) {
        return def->getStateName(state->currentState);
    }
    return empty;
}

// ============================================================
// Example Usage Pattern
// ============================================================

/**
 * Example: Animation system that responds to FSM state changes
 *
 *     void updateAnimationOnFSM(entt::registry& registry) {
 *         auto view = registry.view<FSMStateComponent, AnimationComponent>();
 *         for (auto entity : view) {
 *             auto& fsm = view.get<FSMStateComponent>(entity);
 *             auto& anim = view.get<AnimationComponent>(entity);
 *
 *             if (fsm.justEntered) {
 *                 switch (fsm.currentState) {
 *                     case CharacterStates::Idle:
 *                         anim.play("idle");
 *                         break;
 *                     case CharacterStates::Walk:
 *                         anim.play("walk");
 *                         break;
 *                     case CharacterStates::Attack:
 *                         anim.play("attack");
 *                         break;
 *                 }
 *             }
 *         }
 *     }
 *
 * Example: Input system that sends events to FSM
 *
 *     void processInputForFSM(entt::registry& registry, InputState& input) {
 *         auto view = registry.view<FSMEventQueue, PlayerTag>();
 *         for (auto entity : view) {
 *             auto& events = view.get<FSMEventQueue>(entity);
 *
 *             if (input.isPressed(InputAction::Jump)) {
 *                 events.push("Jump");
 *             }
 *             if (input.isPressed(InputAction::Attack)) {
 *                 events.push("Attack");
 *             }
 *         }
 *     }
 */

} // namespace Vapor
