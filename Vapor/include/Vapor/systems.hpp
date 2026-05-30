#pragma once

#include "components.hpp"
#include "input_manager.hpp"
#include "physics_3d.hpp"
#include "renderer.hpp"
#include "scene.hpp"
#include <entt/entt.hpp>
#include <memory>
#include <random>

namespace Vapor {

    // ============================================================================
    // 變換系統 - 計算世界變換矩陣 (EnTT 版)
    // ============================================================================
    class TransformSystem {
    public:
        static void update(entt::registry& registry) {
            auto view = registry.view<Vapor::TransformComponent>();
            for (auto entity : view) {
                auto& t = view.get<Vapor::TransformComponent>(entity);
                if (!t.isDirty) continue;

                glm::mat4 local = glm::translate(glm::mat4(1.0f), t.position)
                                * glm::mat4_cast(t.rotation)
                                * glm::scale(glm::mat4(1.0f), t.scale);

                if (t.parent != entt::null) {
                    if (auto* parentT = registry.try_get<Vapor::TransformComponent>(t.parent)) {
                        t.worldTransform = parentT->worldTransform * local;
                    } else {
                        t.worldTransform = local;
                    }
                } else {
                    t.worldTransform = local;
                }
                t.isDirty = false;
            }
        }
    };

    // ============================================================================
    // 渲染系統 - 收集渲染實例 (EnTT 版)
    // ============================================================================
    class RenderSystem {
    public:
        struct RenderInstance {
            glm::mat4 model;
            glm::vec4 color;
            Uint32 vertexOffset;
            Uint32 indexOffset;
            Uint32 vertexCount;
            Uint32 indexCount;
            Uint32 materialID;
            PrimitiveMode primitiveMode;
            glm::vec3 AABBMin;
            glm::vec3 AABBMax;
        };

        static void collectInstances(
            entt::registry& registry,
            std::vector<RenderInstance>& instances,
            std::unordered_map<std::shared_ptr<Material>, std::vector<std::shared_ptr<Mesh>>>& instanceBatches
        ) {
            auto view = registry.view<Vapor::TransformComponent, Vapor::MeshRendererComponent>();

            for (auto entity : view) {
                auto& transform = view.get<Vapor::TransformComponent>(entity);
                auto& render = view.get<Vapor::MeshRendererComponent>(entity);

                if (!render.visible) {
                    continue;
                }

                const glm::mat4& modelMatrix = transform.worldTransform;

                for (const auto& mesh : render.meshes) {
                    if (!mesh) continue;
                    
                    instances.push_back(
                        {
                            .model = modelMatrix,
                            .color = glm::vec4(1.0f), // Default color if not in MeshRendererComponent
                            .vertexOffset = mesh->vertexOffset,
                            .indexOffset = mesh->indexOffset,
                            .vertexCount = mesh->vertexCount,
                            .indexCount = mesh->indexCount,
                            .materialID = mesh->materialID,
                            .primitiveMode = mesh->primitiveMode,
                            .AABBMin = mesh->worldAABBMin,
                            .AABBMax = mesh->worldAABBMax,
                        }
                    );

                    if (mesh->material) {
                        instanceBatches[mesh->material].push_back(mesh);
                    }
                }
            }
        }
    };

    // ============================================================================
    // 物理同步系統 - 同步 Transform 和 Physics (EnTT 版)
    // ============================================================================
    class PhysicsSyncSystem {
    public:
        // Scene → Physics: 同步 Kinematic/Static 物體
        static void syncToPhysics(entt::registry& registry, Physics3D* physics) {
            auto view = registry.view<Vapor::TransformComponent, Vapor::RigidbodyComponent>();

            for (auto entity : view) {
                auto& transform = view.get<Vapor::TransformComponent>(entity);
                auto& phys = view.get<Vapor::RigidbodyComponent>(entity);

                if (!phys.body.valid() || !phys.syncToPhysics) {
                    continue;
                }

                // 檢查運動類型
                auto motionType = physics->getMotionType(phys.body);
                if (motionType == BodyMotionType::Kinematic || motionType == BodyMotionType::Static) {
                    physics->setPosition(phys.body, transform.position);
                    physics->setRotation(phys.body, transform.rotation);
                }
            }
        }

        // Physics → Scene: 同步 Dynamic 物體
        static void syncFromPhysics(entt::registry& registry, Physics3D* physics) {
            auto view = registry.view<Vapor::TransformComponent, Vapor::RigidbodyComponent>();

            for (auto entity : view) {
                auto& transform = view.get<Vapor::TransformComponent>(entity);
                auto& phys = view.get<Vapor::RigidbodyComponent>(entity);

                if (!phys.body.valid() || !phys.syncFromPhysics) {
                    continue;
                }

                // 檢查運動類型
                auto motionType = physics->getMotionType(phys.body);
                if (motionType == BodyMotionType::Dynamic) {
                    transform.position = physics->getPosition(phys.body);
                    transform.rotation = physics->getRotation(phys.body);
                    transform.isDirty = true;// 標記需要重新計算世界變換
                }
            }
        }
    };

    // ============================================================================
    // 相機系統 (EnTT 版)
    // ============================================================================
    class CameraSystem {
    public:
        static void update(entt::registry& registry, InputManager& inputManager, float deltaTime) {
            auto view = registry.view<Vapor::VirtualCameraComponent>();
            const auto& inputState = inputManager.getInputState();

            for (auto entity : view) {
                auto& cam = view.get<Vapor::VirtualCameraComponent>(entity);

                if (!cam.isActive) continue;

                // 1. Handle Fly Camera Logic
                if (auto* fly = registry.try_get<FlyCameraComponent>(entity)) {
                    handleFlyCamera(cam, fly, inputState, deltaTime);
                }

                // 2. Handle Follow Camera Logic
                if (auto* follow = registry.try_get<FollowCameraComponent>(entity)) {
                    handleFollowCamera(registry, cam, follow, deltaTime);
                }

                // 3. Update Matrices
                updateMatrices(cam);
            }
        }

        static entt::entity getActiveCamera(entt::registry& registry) {
            auto view = registry.view<Vapor::VirtualCameraComponent>();
            for (auto entity : view) {
                if (view.get<Vapor::VirtualCameraComponent>(entity).isActive) {
                    return entity;
                }
            }
            return entt::null;
        }

    private:
        static void handleFlyCamera(
            VirtualCameraComponent& cam, FlyCameraComponent* fly, const InputState& inputState, float deltaTime
        ) {
            // Rotation
            if (inputState.isPressed(InputAction::LookUp)) fly->pitch += fly->rotateSpeed * deltaTime;
            if (inputState.isPressed(InputAction::LookDown)) fly->pitch -= fly->rotateSpeed * deltaTime;
            if (inputState.isPressed(InputAction::LookLeft)) fly->yaw += fly->rotateSpeed * deltaTime;
            if (inputState.isPressed(InputAction::LookRight)) fly->yaw -= fly->rotateSpeed * deltaTime;

            // Clamp pitch
            fly->pitch = glm::clamp(fly->pitch, -89.0f, 89.0f);

            // Update rotation quaternion from yaw/pitch
            cam.rotation = glm::quat(glm::vec3(glm::radians(-fly->pitch), glm::radians(fly->yaw - 90.0f), 0.0f));

            // Calculate Front/Right/Up vectors from rotation
            glm::vec3 front = cam.rotation * glm::vec3(0, 0, -1);
            glm::vec3 right = cam.rotation * glm::vec3(1, 0, 0);
            glm::vec3 up = cam.rotation * glm::vec3(0, 1, 0);

            // Movement
            float speed = fly->moveSpeed * deltaTime;
            if (inputState.isPressed(InputAction::MoveForward)) cam.position += front * speed;
            if (inputState.isPressed(InputAction::MoveBackward)) cam.position -= front * speed;
            if (inputState.isPressed(InputAction::StrafeLeft)) cam.position -= right * speed;
            if (inputState.isPressed(InputAction::StrafeRight)) cam.position += right * speed;
            if (inputState.isPressed(InputAction::MoveUp)) cam.position += up * speed;
            if (inputState.isPressed(InputAction::MoveDown)) cam.position -= up * speed;
        }

        static void handleFollowCamera(entt::registry& registry, VirtualCameraComponent& cam, FollowCameraComponent* follow, float deltaTime) {
            if (follow->target == entt::null) return;

            // Simple follow logic: Target Position + Offset
            auto* targetTransform = registry.try_get<TransformComponent>(follow->target);
            if (!targetTransform) return;

            glm::vec3 targetPos = targetTransform->position;
            glm::vec3 desiredPos = targetPos + follow->offset;

            // Smooth lerp
            cam.position = glm::mix(cam.position, desiredPos, 1.0f - pow(follow->smoothFactor, deltaTime));

            // Look at target
            cam.rotation = glm::quatLookAt(glm::normalize(targetPos - cam.position), glm::vec3(0, 1, 0));
        }

        static void updateMatrices(VirtualCameraComponent& cam) {
            // View Matrix
            glm::mat4 rotation = glm::mat4_cast(cam.rotation);
            glm::mat4 translation = glm::translate(glm::mat4(1.0f), cam.position);
            cam.viewMatrix = glm::inverse(translation * rotation);

            // Projection Matrix
            cam.projectionMatrix = glm::perspective(cam.fov, cam.aspect, cam.near, cam.far);
        }
    };

    // ============================================================================
    // 粒子力場系統 — attractors + wind → renderer uniforms
    // ============================================================================
    // Call BEFORE renderer->draw(), after TransformSystem::update().
    // Collects ParticleAttractorComponent entities (and emitter entities that
    // flag attractorStrength > 0) and the first ParticleWindComponent, then
    // uploads them to the renderer so the GPU force kernel can use them.
    class ParticleForceFieldSystem {
    public:
        static void update(entt::registry& registry, Renderer* renderer) {
            std::vector<ParticleAttractorData> attractors;

            // Dedicated attractor entities
            auto aView = registry.view<Vapor::TransformComponent, Vapor::ParticleAttractorComponent>();
            for (auto entity : aView) {
                auto& t = aView.get<Vapor::TransformComponent>(entity);
                auto& a = aView.get<Vapor::ParticleAttractorComponent>(entity);
                if (!a.enabled) continue;
                if (attractors.size() >= MAX_PARTICLE_ATTRACTORS) break;
                attractors.push_back({ .position = t.position, .strength = a.strength });
            }

            // Emitter entities also act as attractors (pull particles toward spawn)
            auto eView = registry.view<Vapor::TransformComponent, Vapor::ParticleEmitterComponent>();
            for (auto entity : eView) {
                auto& t = eView.get<Vapor::TransformComponent>(entity);
                auto& e = eView.get<Vapor::ParticleEmitterComponent>(entity);
                if (!e.enabled || e.attractorStrength == 0.0f) continue;
                if (attractors.size() >= MAX_PARTICLE_ATTRACTORS) break;
                attractors.push_back({ .position = t.position, .strength = e.attractorStrength });
            }

            renderer->setParticleAttractors(attractors);

            // Wind — first enabled WindFieldComponent wins
            auto wView = registry.view<Vapor::WindFieldComponent>();
            for (auto entity : wView) {
                auto& w = wView.get<Vapor::WindFieldComponent>(entity);
                if (!w.enabled) continue;
                renderer->setParticleWind(w.direction, w.strength);
                break;
            }
        }
    };

    // ============================================================================
    // 粒子發射系統 — CPU spawn logic → GPU particle slots
    // ============================================================================
    // Call update() once per frame, after TransformSystem::update() and BEFORE
    // renderer->draw().  The system drives CPU-side spawn logic and uploads
    // initial particle state to the GPU; the existing compute shader then takes
    // over velocity integration and attractor forces.
    class ParticleEmitterSystem {
    public:
        static void update(entt::registry& registry, Renderer* renderer, float dt) {
            static std::mt19937 rng{ std::random_device{}() };
            static std::uniform_real_distribution<float> unit{ 0.0f, 1.0f };

            std::vector<ParticleAttractorData> attractors;
            auto view = registry.view<Vapor::TransformComponent, Vapor::ParticleEmitterComponent>();

            for (auto entity : view) {
                auto& t       = view.get<Vapor::TransformComponent>(entity);
                auto& emitter = view.get<Vapor::ParticleEmitterComponent>(entity);
                if (!emitter.enabled) continue;

                // --- Claim GPU slots on first use ---
                if (emitter._slotBegin == ~0u) {
                    emitter._slotCount = emitter.maxParticles;
                    emitter._slotBegin = renderer->claimParticleSlots(emitter._slotCount);
                    if (emitter._slotBegin == ~0u) continue; // pool exhausted
                    emitter._ringCursor = 0;
                }

                // --- Spawn particles this frame ---
                emitter._accumulator += dt;
                float interval = 1.0f / emitter.emissionRate;

                std::vector<GPUParticle> spawns;
                while (emitter._accumulator >= interval) {
                    emitter._accumulator -= interval;

                    // Random velocity inside a cone around emitDirection
                    // using the local-space direction rotated by the entity's world transform.
                    glm::vec3 worldDir = glm::normalize(
                        glm::vec3(t.worldTransform * glm::vec4(emitter.emitDirection, 0.0f))
                    );

                    // Perpendicular basis
                    glm::vec3 perp = std::abs(worldDir.x) < 0.9f
                        ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
                    glm::vec3 u = glm::normalize(glm::cross(worldDir, perp));
                    glm::vec3 v = glm::cross(worldDir, u);

                    float theta = unit(rng) * 2.0f * 3.14159265f;
                    float phi   = unit(rng) * emitter.spread;
                    glm::vec3 dir = glm::normalize(
                        worldDir * std::cos(phi)
                        + (u * std::cos(theta) + v * std::sin(theta)) * std::sin(phi)
                    );

                    GPUParticle p;
                    p.position = t.position;
                    p.velocity = dir * emitter.initialSpeed;
                    p.force    = glm::vec3(0.0f);
                    p.color    = emitter.color;
                    p.lifetime = emitter.particleLifetime;
                    p.age      = 0.0f;

                    spawns.push_back(p);
                }

                if (!spawns.empty()) {
                    // Write into the emitter's ring buffer slice starting at the
                    // current cursor. Ring wrap mid-batch is not split here; it
                    // causes at most one frame of visual glitch at typical rates.
                    uint32_t writeSlot = emitter._slotBegin + emitter._ringCursor;
                    renderer->uploadParticles(writeSlot, spawns);
                    emitter._ringCursor =
                        (emitter._ringCursor + (uint32_t)spawns.size()) % emitter._slotCount;
                }

            }
        }
    };

    // ============================================================================
    // 情緒調製系統 — skeleton（行為留待後續填入）
    // ============================================================================
    // Reads a global EmotionState (stored as a singleton component) and tweaks
    // EmitterModulatorComponent values on all emitter entities each frame.
    class EmitterModulatorSystem {
    public:
        static void update(entt::registry& /*registry*/, EmotionState /*state*/) {
            // TODO: iterate EmitterModulatorComponent, adjust rateMultiplier/
            // colorTint based on state. EmitterModulatorSystem should then
            // be read by ParticleEmitterSystem before it calculates spawn counts.
        }
    };

    // ============================================================================
    // 粒子爆發系統 — skeleton
    // ============================================================================
    // Consumes ParticleBurstRequest tags, fires a one-shot spawn, then removes
    // the component so it only fires once per attachment.
    class ParticleBurstSystem {
    public:
        static void update(entt::registry& /*registry*/, Renderer* /*renderer*/) {
            // TODO: view<TransformComponent, ParticleBurstRequest>, spawn
            // `count` particles in a sphere with `speed` and `spread`, then
            // registry.remove<ParticleBurstRequest>(entity).
        }
    };

}// namespace Vapor
