#pragma once

#include "fsm.hpp"
#include <entt/entt.hpp>

namespace Vapor {

// ============================================================
// FSM System - Emits FSMStateChangeEvent on transition
// ============================================================

/**
 * Initialize FSM for an entity.
 */
inline void initFSM(entt::registry& registry, entt::entity entity,
                    const FSMDefinition& def) {
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
inline void updateFSMSystem(entt::registry& registry, float deltaTime) {
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

// ============================================================
// Helper: Send event to FSM
// ============================================================

inline void sendFSMEvent(entt::registry& registry, entt::entity entity, const std::string& event) {
    auto& events = registry.get_or_emplace<FSMEventQueue>(entity);
    events.push(event);
}

inline void broadcastFSMEvent(entt::registry& registry, const std::string& event) {
    auto view = registry.view<FSMEventQueue>();
    for (auto entity : view) {
        view.get<FSMEventQueue>(entity).push(event);
    }
}

// ============================================================
// Example: Character Effects System (consumes FSMStateChangeEvent)
// ============================================================

/**
 * Example system that responds to FSM state changes and emits Requests.
 *
 *     namespace CharacterStates {
 *         constexpr uint32_t Airborne = 0;
 *         constexpr uint32_t Grounded = 1;
 *         constexpr uint32_t Attack = 2;
 *     }
 *
 *     void CharacterEffectsSystem(entt::registry& reg) {
 *         auto view = reg.view<FSMStateChangeEvent>();
 *         for (auto entity : view) {
 *             auto& event = view.get<FSMStateChangeEvent>(entity);
 *
 *             // Landing: Airborne → Grounded
 *             if (event.fromState == CharacterStates::Airborne &&
 *                 event.toState == CharacterStates::Grounded) {
 *                 reg.emplace<ParticleBurstRequest>(entity, "Dust", 10);
 *                 reg.emplace<SquashRequest>(entity, 1.3f, 0.7f);
 *                 reg.emplace<SoundRequest>(entity, "land");
 *             }
 *
 *             // Attack start
 *             if (event.toState == CharacterStates::Attack) {
 *                 reg.emplace<AnimationRequest>(entity, "attack");
 *                 reg.emplace<CameraTraumaRequest>(entity, 0.2f);
 *             }
 *         }
 *     }
 */

// ============================================================
// Example: Request Consumer Systems
// ============================================================

/**
 * Example: SquashInitSystem - consumes SquashRequest
 *
 *     void SquashInitSystem(entt::registry& reg) {
 *         auto view = reg.view<SquashRequest>();
 *         for (auto entity : view) {
 *             auto& req = view.get<SquashRequest>(entity);
 *             auto& squash = reg.get_or_emplace<SquashStretch>(entity);
 *             squash.currentScale = glm::vec2(req.scaleX, req.scaleY);
 *             squash.velocity = glm::vec2(0.0f);
 *             reg.remove<SquashRequest>(entity);
 *         }
 *     }
 *
 * Example: ParticleSpawnSystem - consumes ParticleBurstRequest
 *
 *     void ParticleSpawnSystem(entt::registry& reg) {
 *         auto view = reg.view<ParticleBurstRequest, Transform>();
 *         for (auto entity : view) {
 *             auto& req = view.get<ParticleBurstRequest>(entity);
 *             auto& transform = view.get<Transform>(entity);
 *
 *             auto emitter = reg.create();
 *             reg.emplace<Transform>(emitter, transform.position + vec3(req.offsetX, req.offsetY, req.offsetZ));
 *             reg.emplace<ParticleBurst>(emitter, req.configName, req.count);
 *             reg.emplace<AutoDestroy>(emitter, 1.0f);
 *
 *             reg.remove<ParticleBurstRequest>(entity);
 *         }
 *     }
 */

// ============================================================
// System Execution Order
// ============================================================

/**
 * Recommended system order:
 *
 *   1. InputManager          : SDL Event → InputState
 *   2. IntentSystem          : InputState → CharacterIntent
 *   3. GroundCheckSystem     : Physics → FSMEventQueue ("Land"/"TakeOff")
 *   4. updateFSMSystem       : FSMEventQueue → FSMStateChangeEvent
 *   5. CharacterEffectsSystem: FSMStateChangeEvent → Request components
 *   6. SquashInitSystem      : SquashRequest → SquashStretch (init)
 *   6. ParticleSpawnSystem   : ParticleBurstRequest → Particle entities
 *   6. SoundPlaySystem       : SoundRequest → Play audio
 *   7. SquashUpdateSystem    : SquashStretch → Transform.scale (continuous)
 *   7. ParticleUpdateSystem  : Particle → Position/Lifetime (continuous)
 *   7. CameraTraumaSystem    : TraumaState → Camera shake (continuous)
 */

} // namespace Vapor
