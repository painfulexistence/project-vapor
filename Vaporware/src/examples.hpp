#pragma once

#include "Vapor/components.hpp"
#include "action_components.hpp"
#include "action_system.hpp"
#include "camera_mixing_system.hpp"
#include "camera_trauma_system.hpp"
#include "components.hpp"
#include "fsm_components.hpp"
#include "fsm_system.hpp"
#include <entt/entt.hpp>
#include <functional>
#include <glm/glm.hpp>
#include <string>

// ============================================================
// PRESETS - Pure data factories for common gameplay patterns
//
// All functions return COMPONENT DATA, they don't emplace.
// Usage: registry.emplace<XxxComponent>(entity, Xxx::preset());
//
// Tween:: presets are in action_components.hpp
// ============================================================

// ============================================================
// 1. ACTION SEQUENCE PRESETS - Returns vector of actions
// ============================================================

namespace ActionSequence {

    // Door open sequence: wait → move up
    inline std::vector<ActionComponent> doorOpen(entt::entity door, const glm::vec3& openPos) {
        return { Action::wait(0.2f), Action::moveTo(door, openPos, 1.0f, Easing::OutCubic) };
    }

    // Attack combo sequence
    inline std::vector<ActionComponent> attackCombo(entt::entity attacker) {
        return { Action::playAnimation(attacker, "slash1"),      Action::wait(0.3f),
                 Action::playAnimation(attacker, "slash2"),      Action::wait(0.3f),
                 Action::playAnimation(attacker, "heavy_slash"), Action::wait(0.4f) };
    }

    // Spawn effect: activate + wait for tween
    inline std::vector<ActionComponent> spawnEffect(entt::entity entity) {
        return { Action::setActive(entity, true), Action::wait(0.5f) };
    }

}// namespace ActionSequence

// ============================================================
// 2. FSM PRESETS - Returns FSMComponent data
// ============================================================

namespace FSM {

    // Simple toggle: Idle <-> Active
    inline FSMComponent
        toggle(entt::entity self, const glm::vec3& idlePos, const glm::vec3& activePos, float duration = 0.5f) {
        return FSMBuilder()
            .state("Idle")
            .enter({ Action::moveTo(self, idlePos, duration, Easing::OutCubic) })
            .transitionTo("Active", "interact")
            .state("Active")
            .enter({ Action::moveTo(self, activePos, duration, Easing::OutCubic) })
            .transitionTo("Idle", "interact")
            .initialState("Idle")
            .build();
    }

    // Combat AI: Idle → Chase → Attack → Cooldown → loop
    inline FSMComponent
        combatAI(entt::entity self, entt::entity target, float detectRange = 10.0f, float attackRange = 2.0f) {
        return FSMBuilder()
            .state("Idle")
            .enter({ Action::playAnimation(self, "idle") })
            .transitionTo("Chase", "player_spotted")
            .transitionTo(
                "Chase",
                [target, detectRange](entt::registry& reg, entt::entity e) {
                    auto* selfTrans = reg.try_get<Vapor::TransformComponent>(e);
                    auto* targetTrans = reg.try_get<Vapor::TransformComponent>(target);
                    if (!selfTrans || !targetTrans) return false;
                    return glm::distance(selfTrans->position, targetTrans->position) < detectRange;
                }
            )

            .state("Chase")
            .enter({ Action::playAnimation(self, "run") })
            .transitionTo(
                "Attack",
                [target, attackRange](entt::registry& reg, entt::entity e) {
                    auto* selfTrans = reg.try_get<Vapor::TransformComponent>(e);
                    auto* targetTrans = reg.try_get<Vapor::TransformComponent>(target);
                    if (!selfTrans || !targetTrans) return false;
                    return glm::distance(selfTrans->position, targetTrans->position) < attackRange;
                }
            )
            .transitionTo("Idle", "player_lost")

            .state("Attack")
            .enter({ Action::playAnimation(self, "attack"), Action::wait(0.5f) })
            .transitionOnComplete("Cooldown")

            .state("Cooldown")
            .enter({ Action::wait(1.0f) })
            .transitionOnComplete("Chase")

            .initialState("Idle")
            .build();
    }

    // Dialogue NPC: Idle → Talking → Waiting → Done
    inline FSMComponent dialogueNPC(entt::entity self) {
        return FSMBuilder()
            .state("Idle")
            .enter({ Action::playAnimation(self, "idle") })
            .transitionTo("Talking", "start_dialogue")

            .state("Talking")
            .enter({ Action::playAnimation(self, "talk") })
            .transitionTo("Waiting", "dialogue_shown")

            .state("Waiting")
            .transitionTo("Talking", "next_dialogue")
            .transitionTo("Done", "end_dialogue")

            .state("Done")
            .enter({ Action::playAnimation(self, "wave") })
            .transitionOnComplete("Idle")

            .initialState("Idle")
            .build();
    }

    // Patrol between two points
    inline FSMComponent patrol(
        entt::entity self,
        const glm::vec3& pointA,
        const glm::vec3& pointB,
        float walkDuration = 2.0f,
        float waitDuration = 1.0f
    ) {
        return FSMBuilder()
            .state("WalkToB")
            .enter({ Action::moveTo(self, pointB, walkDuration, Easing::Linear) })
            .transitionOnComplete("WaitAtB")
            .state("WaitAtB")
            .enter({ Action::wait(waitDuration) })
            .transitionOnComplete("WalkToA")
            .state("WalkToA")
            .enter({ Action::moveTo(self, pointA, walkDuration, Easing::Linear) })
            .transitionOnComplete("WaitAtA")
            .state("WaitAtA")
            .enter({ Action::wait(waitDuration) })
            .transitionOnComplete("WalkToB")
            .initialState("WalkToB")
            .build();
    }

}// namespace FSM

// ============================================================
// 3. LANDING EFFECT (Squash + Particles + Shake)
// ============================================================

struct GroundState {
    bool isGrounded = false;
    bool wasGrounded = false;
};

struct SquashStretchComponent {
    glm::vec2 currentScale{ 1.0f };
    glm::vec2 velocity{ 0.0f };
    float stiffness = 300.0f;
    float damping = 10.0f;
};

struct SquashRequest {
    glm::vec2 impulse{ 0.0f, -0.3f };
};

struct ParticleBurstRequest {
    std::string configName;
    int count = 10;
    glm::vec3 position{ 0.0f };
    float lifetime = 1.0f;
};

struct AutoDestroyComponent {
    float lifetime;
    float elapsed = 0.0f;
};

// Systems
class GroundCheckSystem {
public:
    static void update(entt::registry& reg) {
        auto view = reg.view<GroundState, Vapor::TransformComponent>();
        for (auto entity : view) {
            auto& state = view.get<GroundState>(entity);
            auto& transform = view.get<Vapor::TransformComponent>(entity);

            // Detect landing (false → true)
            if (state.isGrounded && !state.wasGrounded) {
                // Squash effect
                reg.emplace_or_replace<SquashRequest>(entity, glm::vec2(0.3f, -0.3f));

                // Camera shake
                CameraTraumaSystem::addTraumaToActiveCamera(reg, TraumaPresets::lightImpact());

                // Spawn dust particles
                auto emitter = reg.create();
                reg.emplace<ParticleBurstRequest>(emitter, "dust", 10, transform.position, 1.0f);
                reg.emplace<AutoDestroyComponent>(emitter, 1.0f);
            }

            state.wasGrounded = state.isGrounded;
        }
    }
};

class SquashInitSystem {
public:
    static void update(entt::registry& reg) {
        auto view = reg.view<SquashRequest>();
        for (auto entity : view) {
            auto& request = view.get<SquashRequest>(entity);
            auto& squash = reg.get_or_emplace<SquashStretchComponent>(entity);
            squash.velocity += request.impulse;
        }
        reg.clear<SquashRequest>();
    }
};

class SquashUpdateSystem {
public:
    static void update(entt::registry& reg, float dt) {
        auto view = reg.view<SquashStretchComponent, Vapor::TransformComponent>();
        for (auto entity : view) {
            auto& squash = view.get<SquashStretchComponent>(entity);
            auto& transform = view.get<Vapor::TransformComponent>(entity);

            // Spring physics (Hooke's Law)
            glm::vec2 target(1.0f);
            glm::vec2 force = (target - squash.currentScale) * squash.stiffness;
            squash.velocity += force * dt;
            squash.velocity *= (1.0f - squash.damping * dt);
            squash.currentScale += squash.velocity * dt;

            // Apply to transform
            transform.scale.x = squash.currentScale.x;
            transform.scale.y = squash.currentScale.y;
            transform.isDirty = true;
        }
    }
};

class AutoDestroySystem {
public:
    static void update(entt::registry& reg, float dt) {
        auto view = reg.view<AutoDestroyComponent>();
        std::vector<entt::entity> toDestroy;

        for (auto entity : view) {
            auto& autoDestroy = view.get<AutoDestroyComponent>(entity);
            autoDestroy.elapsed += dt;
            if (autoDestroy.elapsed >= autoDestroy.lifetime) {
                toDestroy.push_back(entity);
            }
        }

        for (auto entity : toDestroy) {
            reg.destroy(entity);
        }
    }
};

// ============================================================
// 4. SCENE MANAGEMENT
// ============================================================

enum class SceneTransitionType : uint8_t { None, Fade, Crossfade, Wipe };

struct SceneRequest {
    enum class ActionType : uint8_t { Load, Unload, Reload };

    ActionType action = ActionType::Load;
    std::string sceneId;
    bool unloadCurrent = true;
    SceneTransitionType transition = SceneTransitionType::Fade;
    float transitionDuration = 0.5f;
    glm::vec3 spawnPosition{ 0.0f };
    std::string spawnPointId;
};

struct SceneState {
    std::string currentSceneId;
    std::string pendingSceneId;
    bool isTransitioning = false;

    enum class Phase : uint8_t { Idle, FadingOut, Loading, FadingIn } phase = Phase::Idle;

    float transitionProgress = 0.0f;
    float transitionDuration = 0.5f;
};

struct SceneTriggerZone {
    std::string targetSceneId;
    std::string spawnPointId;
    glm::vec3 spawnPosition{ 0.0f };
    bool useSpawnPoint = true;
    bool triggered = false;
};

struct PlayerContact {
    entt::entity player = entt::null;
};

// ============================================================
// Scene Systems - Each system has ONE update, ONE responsibility
// ============================================================

namespace SceneHelpers {
    inline void loadScene(entt::registry& reg, const std::string& sceneId) {
        fmt::print("Loading scene: {}\n", sceneId);
    }

    inline void unloadScene(entt::registry& reg, const std::string& sceneId) {
        fmt::print("Unloading scene: {}\n", sceneId);
    }
}// namespace SceneHelpers

class SceneTriggerSystem {
public:
    static void update(entt::registry& reg) {
        auto view = reg.view<SceneTriggerZone, PlayerContact>();
        for (auto entity : view) {
            auto& trigger = view.get<SceneTriggerZone>(entity);
            auto& contact = view.get<PlayerContact>(entity);

            if (contact.player != entt::null && !trigger.triggered) {
                trigger.triggered = true;

                auto reqEntity = reg.create();
                auto& request = reg.emplace<SceneRequest>(reqEntity);
                request.action = SceneRequest::ActionType::Load;
                request.sceneId = trigger.targetSceneId;
                request.unloadCurrent = true;
                request.transition = SceneTransitionType::Fade;
                if (trigger.useSpawnPoint) {
                    request.spawnPointId = trigger.spawnPointId;
                } else {
                    request.spawnPosition = trigger.spawnPosition;
                }
            }

            reg.remove<PlayerContact>(entity);
        }
    }
};

class SceneRequestSystem {
public:
    static void update(entt::registry& reg) {
        SceneState* sceneState = nullptr;
        auto stateView = reg.view<SceneState>();
        if (stateView.begin() != stateView.end()) {
            sceneState = &reg.get<SceneState>(*stateView.begin());
        }
        if (!sceneState) return;

        if (sceneState->isTransitioning) return;

        auto view = reg.view<SceneRequest>();
        for (auto entity : view) {
            auto& request = view.get<SceneRequest>(entity);

            switch (request.action) {
            case SceneRequest::ActionType::Load:
                sceneState->pendingSceneId = request.sceneId;
                sceneState->isTransitioning = true;
                sceneState->phase = SceneState::Phase::FadingOut;
                sceneState->transitionProgress = 0.0f;
                sceneState->transitionDuration = request.transitionDuration;
                break;

            case SceneRequest::ActionType::Unload:
                SceneHelpers::unloadScene(reg, request.sceneId);
                break;

            case SceneRequest::ActionType::Reload:
                sceneState->pendingSceneId = sceneState->currentSceneId;
                sceneState->isTransitioning = true;
                sceneState->phase = SceneState::Phase::FadingOut;
                break;
            }

            reg.destroy(entity);
            break;
        }
    }
};

class SceneTransitionSystem {
public:
    static void update(entt::registry& reg, float dt) {
        auto view = reg.view<SceneState>();
        for (auto entity : view) {
            auto& state = view.get<SceneState>(entity);
            if (!state.isTransitioning) continue;

            state.transitionProgress += dt / state.transitionDuration;

            switch (state.phase) {
            case SceneState::Phase::FadingOut:
                if (state.transitionProgress >= 1.0f) {
                    state.phase = SceneState::Phase::Loading;
                    state.transitionProgress = 0.0f;
                }
                break;

            case SceneState::Phase::Loading:
                SceneHelpers::unloadScene(reg, state.currentSceneId);
                SceneHelpers::loadScene(reg, state.pendingSceneId);
                state.currentSceneId = state.pendingSceneId;
                state.pendingSceneId.clear();
                state.phase = SceneState::Phase::FadingIn;
                state.transitionProgress = 0.0f;
                break;

            case SceneState::Phase::FadingIn:
                if (state.transitionProgress >= 1.0f) {
                    state.phase = SceneState::Phase::Idle;
                    state.isTransitioning = false;
                }
                break;

            default:
                break;
            }
        }
    }
};

// ============================================================
// GAME LOOP EXAMPLE - Complete System Order
// ============================================================

namespace GameLoopExample {

    inline void update(entt::registry& reg, float dt) {
        // ===== 1. INPUT =====
        // inputManager.update(dt);

        // ===== 2. STATE CHANGE DETECTION =====
        GroundCheckSystem::update(reg);

        // ===== 3. SCENE MANAGEMENT =====
        SceneTriggerSystem::update(reg);
        SceneRequestSystem::update(reg);
        SceneTransitionSystem::update(reg, dt);

        // ===== 4. REQUEST CONSUMERS =====
        SquashInitSystem::update(reg);

        // ===== 5. FSM (State transitions → emplace ActionQueue) =====
        FSMSystem::update(reg);

        // ===== 6. ACTION EXECUTION =====
        ActionSystem::update(reg, dt);

        // ===== 7. CONTINUOUS UPDATE SYSTEMS =====
        SquashUpdateSystem::update(reg, dt);
        CameraTraumaSystem::update(reg, dt);
        CameraBreathSystem::update(reg, dt);

        // ===== 8. CLEANUP =====
        AutoDestroySystem::update(reg, dt);

        // ===== 9. PHYSICS =====
        // physics->process(scene, dt);

        // ===== 10. RENDER =====
        // Camera finalCamera = CameraMixingSystem::resolve(reg);
        // renderer->draw(scene, finalCamera);
    }

}// namespace GameLoopExample
