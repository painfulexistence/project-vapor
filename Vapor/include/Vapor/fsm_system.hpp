#pragma once

#include "fsm.hpp"
#include "components.hpp"
#include <entt/entt.hpp>

namespace Vapor {

// ============================================================
// FSM Init System - automatically initializes FSM for entities
// ============================================================
// Detects entities with FSMDefinition but no FSMStateComponent,
// and initializes them. Run before FSMSystem::update().

class FSMInitSystem {
public:
    static void update(entt::registry& registry) {
        auto view = registry.view<FSMDefinition>(entt::exclude<FSMStateComponent, InactiveComponent>);

        for (auto entity : view) {
            auto& def = view.get<FSMDefinition>(entity);
            auto& state = registry.emplace<FSMStateComponent>(entity);
            state.currentState = def.initialState;
            state.stateTime = 0.0f;
            state.totalTime = 0.0f;

            // Emit initial state enter event
            registry.emplace_or_replace<FSMStateChangeEvent>(entity,
                FSMStateChangeEvent{ def.initialState, def.initialState, 0.0f });
        }
    }
};

// ============================================================
// FSM System - state machine update logic
// ============================================================

class FSMSystem {
public:
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

        auto view = registry.view<FSMStateComponent, FSMDefinition>(entt::exclude<InactiveComponent>);

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
};

} // namespace Vapor
