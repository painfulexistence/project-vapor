#pragma once

#include "animation_components.hpp"
#include "animation_systems.hpp"
#include "camera_mixing_system.hpp"
#include "camera_trauma_system.hpp"
#include "components.hpp"
#include "fsm_components.hpp"
#include "fsm_system.hpp"
#include "Vapor/components.hpp"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <functional>
#include <string>

// ============================================================
// PRESETS - Pure data factories for common gameplay patterns
//
// All functions return COMPONENT DATA, they don't emplace.
// Usage: registry.emplace<XxxComponent>(entity, Xxx::preset());
// ============================================================

// ============================================================
// 1. TWEEN PRESETS - Returns TweenComponent data
// ============================================================

namespace Tween {

    // Bounce in effect (scale from 0 to 1 with overshoot)
    inline TweenTransformComponent bounceIn(float duration = 0.5f) {
        TweenTransformComponent tween;
        tween.base.duration = duration;
        tween.base.easing = Easing::OutBack;
        tween.base.state = TweenState::Running;
        tween.target = TweenTransformTarget::Scale;
        tween.startScale = glm::vec3(0.0f);
        tween.endScale = glm::vec3(1.0f);
        return tween;
    }

    // Bounce out effect (scale from 1 to 0)
    inline TweenTransformComponent bounceOut(float duration = 0.3f) {
        TweenTransformComponent tween;
        tween.base.duration = duration;
        tween.base.easing = Easing::InBack;
        tween.base.state = TweenState::Running;
        tween.target = TweenTransformTarget::Scale;
        tween.startScale = glm::vec3(1.0f);
        tween.endScale = glm::vec3(0.0f);
        return tween;
    }

    // Pulsing scale (looping)
    inline TweenTransformComponent pulse(float minScale = 0.9f, float maxScale = 1.1f, float duration = 1.0f) {
        TweenTransformComponent tween;
        tween.base.duration = duration;
        tween.base.easing = Easing::InOutQuad;
        tween.base.state = TweenState::Running;
        tween.base.loopMode = TweenLoopMode::PingPong;
        tween.base.loopCount = -1;  // Infinite
        tween.target = TweenTransformTarget::Scale;
        tween.startScale = glm::vec3(minScale);
        tween.endScale = glm::vec3(maxScale);
        return tween;
    }

    // Fade out (alpha 1 → 0)
    inline TweenColorComponent fadeOut(float duration = 0.3f) {
        TweenColorComponent tween;
        tween.base.duration = duration;
        tween.base.easing = Easing::OutCubic;
        tween.base.state = TweenState::Running;
        tween.startValue = glm::vec4(1.0f);
        tween.endValue = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        return tween;
    }

    // Fade in (alpha 0 → 1)
    inline TweenColorComponent fadeIn(float duration = 0.3f) {
        TweenColorComponent tween;
        tween.base.duration = duration;
        tween.base.easing = Easing::OutCubic;
        tween.base.state = TweenState::Running;
        tween.startValue = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        tween.endValue = glm::vec4(1.0f);
        return tween;
    }

    // Move from A to B
    inline TweenTransformComponent move(const glm::vec3& from, const glm::vec3& to,
                                         float duration = 0.5f, EasingFunction easing = Easing::OutCubic) {
        TweenTransformComponent tween;
        tween.base.duration = duration;
        tween.base.easing = easing;
        tween.base.state = TweenState::Running;
        tween.target = TweenTransformTarget::Position;
        tween.startPosition = from;
        tween.endPosition = to;
        return tween;
    }

}  // namespace Tween

// ============================================================
// 2. ACTION SEQUENCE PRESETS - Returns vector of actions
// ============================================================

namespace ActionSequence {
    using namespace AnimationBuilder;

    // Door open sequence: wait → move up
    inline std::vector<TimelineAction> doorOpen(entt::entity door, const glm::vec3& openPos) {
        return {
            wait(0.2f),
            moveTo(door, openPos, 1.0f, Easing::OutCubic)
        };
    }

    // Attack combo sequence
    inline std::vector<TimelineAction> attackCombo(entt::entity attacker) {
        return {
            playAnimation(attacker, "slash1"),
            wait(0.3f),
            playAnimation(attacker, "slash2"),
            wait(0.3f),
            playAnimation(attacker, "heavy_slash"),
            wait(0.4f)
        };
    }

    // Spawn effect: scale up + fade in
    inline std::vector<TimelineAction> spawnEffect(entt::entity entity) {
        return {
            setActive(entity, true),
            wait(0.5f)  // Let tween play
        };
    }

}  // namespace ActionSequence

// ============================================================
// 3. FSM PRESETS - Returns FSMComponent data
// ============================================================

namespace FSM {

    // Simple toggle: Idle <-> Active
    inline FSMComponent toggle(entt::entity self,
                                const glm::vec3& idlePos,
                                const glm::vec3& activePos,
                                float duration = 0.5f) {
        using namespace AnimationBuilder;

        return FSMBuilder()
            .state("Idle")
                .enter({ moveTo(self, idlePos, duration, Easing::OutCubic) })
                .transitionTo("Active", "interact")
            .state("Active")
                .enter({ moveTo(self, activePos, duration, Easing::OutCubic) })
                .transitionTo("Idle", "interact")
            .initialState("Idle")
            .build();
    }

    // Combat AI: Idle → Chase → Attack → Cooldown → loop
    inline FSMComponent combatAI(entt::entity self, entt::entity target,
                                  float detectRange = 10.0f, float attackRange = 2.0f) {
        using namespace AnimationBuilder;

        return FSMBuilder()
            .state("Idle")
                .enter({ playAnimation(self, "idle") })
                .transitionTo("Chase", "player_spotted")
                .transitionTo("Chase", [target, detectRange](entt::registry& reg, entt::entity e) {
                    auto* selfTrans = reg.try_get<Vapor::TransformComponent>(e);
                    auto* targetTrans = reg.try_get<Vapor::TransformComponent>(target);
                    if (!selfTrans || !targetTrans) return false;
                    return glm::distance(selfTrans->position, targetTrans->position) < detectRange;
                })

            .state("Chase")
                .enter({ playAnimation(self, "run") })
                .transitionTo("Attack", [target, attackRange](entt::registry& reg, entt::entity e) {
                    auto* selfTrans = reg.try_get<Vapor::TransformComponent>(e);
                    auto* targetTrans = reg.try_get<Vapor::TransformComponent>(target);
                    if (!selfTrans || !targetTrans) return false;
                    return glm::distance(selfTrans->position, targetTrans->position) < attackRange;
                })
                .transitionTo("Idle", "player_lost")

            .state("Attack")
                .enter({
                    playAnimation(self, "attack"),
                    wait(0.5f)
                })
                .transitionOnComplete("Cooldown")

            .state("Cooldown")
                .enter({ wait(1.0f) })
                .transitionOnComplete("Chase")

            .initialState("Idle")
            .build();
    }

    // Dialogue NPC: Idle → Talking → Waiting → Done
    inline FSMComponent dialogueNPC(entt::entity self) {
        using namespace AnimationBuilder;

        return FSMBuilder()
            .state("Idle")
                .enter({ playAnimation(self, "idle") })
                .transitionTo("Talking", "start_dialogue")

            .state("Talking")
                .enter({ playAnimation(self, "talk") })
                .transitionTo("Waiting", "dialogue_shown")

            .state("Waiting")
                .transitionTo("Talking", "next_dialogue")
                .transitionTo("Done", "end_dialogue")

            .state("Done")
                .enter({ playAnimation(self, "wave") })
                .transitionOnComplete("Idle")

            .initialState("Idle")
            .build();
    }

    // Patrol between two points
    inline FSMComponent patrol(entt::entity self,
                                const glm::vec3& pointA,
                                const glm::vec3& pointB,
                                float walkDuration = 2.0f,
                                float waitDuration = 1.0f) {
        using namespace AnimationBuilder;

        return FSMBuilder()
            .state("WalkToB")
                .enter({ moveTo(self, pointB, walkDuration, Easing::Linear) })
                .transitionOnComplete("WaitAtB")
            .state("WaitAtB")
                .enter({ wait(waitDuration) })
                .transitionOnComplete("WalkToA")
            .state("WalkToA")
                .enter({ moveTo(self, pointA, walkDuration, Easing::Linear) })
                .transitionOnComplete("WaitAtA")
            .state("WaitAtA")
                .enter({ wait(waitDuration) })
                .transitionOnComplete("WalkToB")
            .initialState("WalkToB")
            .build();
    }

}  // namespace FSM

// ============================================================
// 4. LANDING EFFECT (Squash + Particles + Shake)
// ============================================================

// Components for landing system
struct GroundState {
    bool isGrounded = false;
    bool wasGrounded = false;
};

struct SquashStretchComponent {
    glm::vec2 currentScale{1.0f};
    glm::vec2 velocity{0.0f};
    float stiffness = 300.0f;
    float damping = 10.0f;
};

struct SquashRequest {
    glm::vec2 impulse{0.0f, -0.3f};
};

struct ParticleBurstRequest {
    std::string configName;
    int count = 10;
    glm::vec3 position{0.0f};
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
// 5. CUTSCENE EXAMPLE (Multi-entity Coordination)
// ============================================================

namespace CutsceneExamples {

    // Intro cutscene: Camera pan → NPC walks in → Dialogue
    inline void playIntroCutscene(entt::registry& reg,
                                   entt::entity camera,
                                   entt::entity player,
                                   entt::entity npc) {
        using namespace AnimationBuilder;

        // Create a cutscene controller entity
        auto cutsceneController = reg.create();

        AnimationHelpers::playCutscene(reg, cutsceneController, {
            // Disable player control
            callback([&reg, player]() {
                if (auto* intent = reg.try_get<CharacterIntent>(player)) {
                    // Could add a "CutsceneControlled" tag to disable input
                }
            }),

            // Camera pan to scene
            moveTo(camera, glm::vec3(0, 5, 10), 2.0f, Easing::InOutQuad),

            wait(0.5f),

            // NPC walks into frame
            parallel({
                moveTo(npc, glm::vec3(0, 0, 0), 1.5f, Easing::OutQuad),
                playAnimation(npc, "walk")
            }),

            // NPC stops and turns
            playAnimation(npc, "idle"),
            wait(0.3f),

            // Start dialogue
            callback([&reg, npc]() {
                FSMSystem::sendEvent(reg, npc, "start_dialogue");
            }),

            // Wait for dialogue to finish (manual advance)
            wait(5.0f),  // Or use a condition-based wait

            // Camera returns to player
            moveTo(camera, glm::vec3(0, 2, 5), 1.0f, Easing::InOutQuad),

            // Re-enable player control
            callback([&reg, player]() {
                // Remove "CutsceneControlled" tag
            })
        }, [&reg, cutsceneController]() {
            // Cleanup cutscene controller
            reg.destroy(cutsceneController);
        }, "intro_cutscene");
    }

    // Boss intro: Screen shake → Boss lands → Roar → Health bar appears
    inline void playBossIntro(entt::registry& reg,
                               entt::entity boss,
                               entt::entity camera,
                               entt::entity healthBarUI) {
        using namespace AnimationBuilder;

        auto cutsceneController = reg.create();

        AnimationHelpers::playCutscene(reg, cutsceneController, {
            // Rumble warning
            callback([&reg]() {
                CameraTraumaSystem::addTraumaToActiveCamera(reg, CameraTraumaRequest::shake(0.3f));
            }),
            wait(1.0f),

            // Boss drops from sky
            setActive(boss, true),
            moveTo(boss, glm::vec3(0, 0, 0), 0.5f, Easing::InCubic),

            // Impact!
            callback([&reg]() {
                CameraTraumaSystem::addTraumaToActiveCamera(reg, TraumaPresets::heavyImpact());
            }),
            wait(0.3f),

            // Boss roar animation
            playAnimation(boss, "roar"),
            wait(2.0f),

            // Show health bar
            setActive(healthBarUI, true),
            callback([&reg, healthBarUI]() {
                TweenExamples::bounceIn(reg, healthBarUI, 0.5f);
            }),
            wait(0.5f),

            // Boss enters combat stance
            playAnimation(boss, "combat_idle")
        }, [&reg, cutsceneController]() {
            reg.destroy(cutsceneController);
        }, "boss_intro");
    }

}  // namespace CutsceneExamples

// ============================================================
// 6. SCENE MANAGER SYSTEM
// ============================================================

// Scene transition types
enum class SceneTransitionType : uint8_t {
    None,
    Fade,
    Crossfade,
    Wipe
};

// Scene request component
struct SceneRequest {
    enum class Action : uint8_t {
        Load,
        Unload,
        Reload
    };

    Action action = Action::Load;
    std::string sceneId;
    bool unloadCurrent = true;
    SceneTransitionType transition = SceneTransitionType::Fade;
    float transitionDuration = 0.5f;
    glm::vec3 spawnPosition{0.0f};      // Where to spawn player
    std::string spawnPointId;            // Or use named spawn point
};

// Scene state component (on a global/manager entity)
struct SceneState {
    std::string currentSceneId;
    std::string pendingSceneId;
    bool isTransitioning = false;

    enum class Phase : uint8_t {
        Idle,
        FadingOut,
        Loading,
        FadingIn
    } phase = Phase::Idle;

    float transitionProgress = 0.0f;
    float transitionDuration = 0.5f;
};

// Trigger zone component
struct SceneTriggerZone {
    std::string targetSceneId;
    std::string spawnPointId;
    glm::vec3 spawnPosition{0.0f};
    bool useSpawnPoint = true;
    bool triggered = false;
};

// Player contact detection (set by physics/collision system)
struct PlayerContact {
    entt::entity player = entt::null;
};

// ============================================================
// Scene Systems - Each system has ONE update, ONE responsibility
// ============================================================

// Helper functions for scene loading (not systems)
namespace SceneHelpers {
    inline void loadScene(entt::registry& reg, const std::string& sceneId) {
        // TODO: Implement actual scene loading
        fmt::print("Loading scene: {}\n", sceneId);
    }

    inline void unloadScene(entt::registry& reg, const std::string& sceneId) {
        // TODO: Implement actual scene unloading
        fmt::print("Unloading scene: {}\n", sceneId);
    }
}

// System 1: Detects trigger zones → emplaces SceneRequest
class SceneTriggerSystem {
public:
    static void update(entt::registry& reg) {
        auto view = reg.view<SceneTriggerZone, PlayerContact>();
        for (auto entity : view) {
            auto& trigger = view.get<SceneTriggerZone>(entity);
            auto& contact = view.get<PlayerContact>(entity);

            if (contact.player != entt::null && !trigger.triggered) {
                trigger.triggered = true;

                // Emplace scene request
                auto reqEntity = reg.create();
                auto& request = reg.emplace<SceneRequest>(reqEntity);
                request.action = SceneRequest::Action::Load;
                request.sceneId = trigger.targetSceneId;
                request.unloadCurrent = true;
                request.transition = SceneTransitionType::Fade;
                if (trigger.useSpawnPoint) {
                    request.spawnPointId = trigger.spawnPointId;
                } else {
                    request.spawnPosition = trigger.spawnPosition;
                }
            }

            // Clear contact after processing
            reg.remove<PlayerContact>(entity);
        }
    }
};

// System 2: Consumes SceneRequest → starts transition in SceneState
class SceneRequestSystem {
public:
    static void update(entt::registry& reg) {
        // Find scene state
        SceneState* sceneState = nullptr;
        auto stateView = reg.view<SceneState>();
        if (stateView.begin() != stateView.end()) {
            sceneState = &reg.get<SceneState>(*stateView.begin());
        }
        if (!sceneState) return;

        // Don't process new requests while transitioning
        if (sceneState->isTransitioning) return;

        auto view = reg.view<SceneRequest>();
        for (auto entity : view) {
            auto& request = view.get<SceneRequest>(entity);

            switch (request.action) {
                case SceneRequest::Action::Load:
                    sceneState->pendingSceneId = request.sceneId;
                    sceneState->isTransitioning = true;
                    sceneState->phase = SceneState::Phase::FadingOut;
                    sceneState->transitionProgress = 0.0f;
                    sceneState->transitionDuration = request.transitionDuration;
                    break;

                case SceneRequest::Action::Unload:
                    SceneHelpers::unloadScene(reg, request.sceneId);
                    break;

                case SceneRequest::Action::Reload:
                    sceneState->pendingSceneId = sceneState->currentSceneId;
                    sceneState->isTransitioning = true;
                    sceneState->phase = SceneState::Phase::FadingOut;
                    break;
            }

            // Consume request
            reg.destroy(entity);
            break;  // One request at a time
        }
    }
};

// System 3: Updates transition animation based on SceneState.phase
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
// 7. COMPLETE TRIGGER ZONE → SCENE TRANSITION EXAMPLE
// ============================================================

namespace SceneTransitionExample {

    // Setup a trigger zone that transitions to another scene
    inline entt::entity createSceneTrigger(entt::registry& reg,
                                            const glm::vec3& position,
                                            const glm::vec3& size,
                                            const std::string& targetScene,
                                            const std::string& spawnPoint = "") {
        auto trigger = reg.create();

        // Transform
        auto& transform = reg.emplace<Vapor::TransformComponent>(trigger);
        transform.position = position;
        transform.scale = size;

        // Trigger zone
        auto& zone = reg.emplace<SceneTriggerZone>(trigger);
        zone.targetSceneId = targetScene;
        zone.spawnPointId = spawnPoint;
        zone.useSpawnPoint = !spawnPoint.empty();

        // Collider (for physics system to detect)
        auto& collider = reg.emplace<Vapor::BoxColliderComponent>(trigger);
        collider.halfSize = size * 0.5f;

        return trigger;
    }

    // Setup scene manager
    inline entt::entity setupSceneManager(entt::registry& reg, const std::string& initialScene) {
        auto manager = reg.create();

        auto& state = reg.emplace<SceneState>(manager);
        state.currentSceneId = initialScene;

        return manager;
    }

    // Example: Complete game loop integration
    inline void exampleGameLoop(entt::registry& reg, float dt) {
        // ... input handling ...
        // ... physics (sets PlayerContact on trigger zones) ...

        // Scene management (each system has one responsibility)
        SceneTriggerSystem::update(reg);      // Trigger → SceneRequest
        SceneRequestSystem::update(reg);      // SceneRequest → start transition
        SceneTransitionSystem::update(reg, dt); // Update transition animation

        // ... rest of game loop ...
    }

    // Example: Creating a level with trigger zones
    inline void setupLevel(entt::registry& reg) {
        // Scene manager
        setupSceneManager(reg, "level_1");

        // Exit to level 2
        createSceneTrigger(reg,
            glm::vec3(50, 0, 0),    // Position
            glm::vec3(2, 4, 2),     // Size
            "level_2",              // Target scene
            "spawn_from_level_1"    // Spawn point
        );

        // Secret area
        createSceneTrigger(reg,
            glm::vec3(10, -5, 0),
            glm::vec3(2, 2, 2),
            "secret_area",
            "spawn_main"
        );

        // Return to hub
        createSceneTrigger(reg,
            glm::vec3(-50, 0, 0),
            glm::vec3(3, 4, 3),
            "hub_world",
            "spawn_from_level_1"
        );
    }

}  // namespace SceneTransitionExample

// ============================================================
// GAME LOOP EXAMPLE - Complete System Order
// ============================================================

namespace GameLoopExample {

    inline void update(entt::registry& reg, float dt) {
        // ===== 1. INPUT =====
        // inputManager.update(dt);
        // updateCharacterIntent(reg, inputState);

        // ===== 2. STATE CHANGE DETECTION =====
        GroundCheckSystem::update(reg);
        // CollisionEventSystem::update(reg);

        // ===== 3. SCENE MANAGEMENT (3 systems, 3 responsibilities) =====
        SceneTriggerSystem::update(reg);        // Trigger → SceneRequest
        SceneRequestSystem::update(reg);        // SceneRequest → start transition
        SceneTransitionSystem::update(reg, dt); // Update transition animation

        // ===== 4. REQUEST CONSUMERS (Init systems) =====
        SquashInitSystem::update(reg);
        // ParticleSpawnSystem::update(reg);

        // ===== 5. FSM (State transitions → emplace ActionQueue) =====
        FSMSystem::update(reg);

        // ===== 6. ANIMATION EXECUTION =====
        AnimationSystem::update(reg, dt);

        // ===== 7. CONTINUOUS UPDATE SYSTEMS =====
        SquashUpdateSystem::update(reg, dt);
        CameraTraumaSystem::update(reg, dt);
        CameraBreathSystem::update(reg, dt);
        // ParticleUpdateSystem::update(reg, dt);

        // ===== 8. CLEANUP =====
        AutoDestroySystem::update(reg, dt);

        // ===== 9. PHYSICS =====
        // physics->process(scene, dt);

        // ===== 10. RENDER =====
        // Camera finalCamera = CameraMixingSystem::resolve(reg);
        // renderer->draw(scene, finalCamera);
    }

}  // namespace GameLoopExample
