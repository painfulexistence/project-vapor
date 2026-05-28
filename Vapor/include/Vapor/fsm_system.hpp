#pragma once

#include "fsm.hpp"
#include <entt/entt.hpp>

namespace Vapor {

// ============================================================
// FSM System - follows static void update pattern
// ============================================================

class FSMSystem {
public:
    /**
     * Initialize FSM for an entity.
     */
    static void init(entt::registry& registry, entt::entity entity, const FSMDefinition& def) {
        auto& state = registry.get_or_emplace<FSMStateComponent>(entity);
        state.currentState = def.initialState;
        state.stateTime = 0.0f;
        state.totalTime = 0.0f;

        // Emit initial state enter event
        registry.emplace_or_replace<FSMStateChangeEvent>(entity,
            FSMStateChangeEvent{ def.initialState, def.initialState, 0.0f });
    }

    /**
     * Main FSM update system.
     *
     * Pipeline:
     *   1. Clear previous frame's FSMStateChangeEvent
     *   2. Process FSMEventQueue
     *   3. Check timed transitions
     *   4. Emit FSMStateChangeEvent if state changed
     *   5. Update timers
     */
    static void update(entt::registry& registry, float deltaTime) {
        // 1. Clear previous frame's state change events
        registry.clear<FSMStateChangeEvent>();

        auto view = registry.view<FSMStateComponent, FSMDefinition>();

        for (auto entity : view) {
            auto& state = view.get<FSMStateComponent>(entity);
            auto& def = view.get<FSMDefinition>(entity);

            uint32_t previousState = state.currentState;
            float previousStateTime = state.stateTime;
            bool transitioned = false;

            // 2. Process events
            if (auto* events = registry.try_get<FSMEventQueue>(entity)) {
                for (const auto& event : events->events) {
                    if (transitioned) break;

                    for (const auto& rule : def.eventTransitions) {
                        if (rule.fromState == state.currentState &&
                            rule.triggerEvent == event &&
                            state.stateTime >= rule.minStateTime) {
                            state.currentState = rule.toState;
                            state.stateTime = 0.0f;
                            transitioned = true;
                            break;
                        }
                    }
                }
                events->clear();
            }

            // 3. Check timed transitions
            if (!transitioned) {
                for (const auto& timed : def.timedTransitions) {
                    if (timed.fromState == state.currentState &&
                        state.stateTime >= timed.duration) {
                        state.currentState = timed.toState;
                        state.stateTime = 0.0f;
                        transitioned = true;
                        break;
                    }
                }
            }

            // 4. Emit state change event
            if (transitioned) {
                registry.emplace<FSMStateChangeEvent>(entity,
                    FSMStateChangeEvent{ previousState, state.currentState, previousStateTime });
            }

            // 5. Update timers
            state.stateTime += deltaTime;
            state.totalTime += deltaTime;
        }
    }

    /**
     * Send an event to a specific entity's FSM.
     */
    static void sendEvent(entt::registry& registry, entt::entity entity, const std::string& event) {
        auto& events = registry.get_or_emplace<FSMEventQueue>(entity);
        events.push(event);
    }

    /**
     * Broadcast an event to all entities with FSMEventQueue.
     */
    static void broadcastEvent(entt::registry& registry, const std::string& event) {
        auto view = registry.view<FSMEventQueue>();
        for (auto entity : view) {
            view.get<FSMEventQueue>(entity).push(event);
        }
    }
};

} // namespace Vapor
