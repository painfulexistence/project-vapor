#pragma once

#include "components.hpp"
#include "input_manager.hpp"
#include "physics_3d.hpp"
#include "renderer.hpp"
#include "scene.hpp"
// #include "world.hpp"  // TODO: world.hpp not found - ECS World class may be needed
#include <entt/entt.hpp>
#include <memory>

// Temporary alias for compatibility - replace with actual World class when available
using World = entt::registry;

namespace Vapor {

    // ============================================================================
    // 變換系統 - 計算世界變換矩陣
    // ============================================================================
    class TransformSystem {
    public:
        static void update(World& world) {
            auto* transformPool = world.GetPool<TransformComponent>();

            // 第一遍：標記所有需要更新的實體
            for (size_t i = 0; i < transformPool->components.size(); ++i) {
                Entity e = transformPool->denseToEntity[i];
                auto& transform = transformPool->components[i];

                if (transform.isDirty || transform.parent != NULL_ENTITY) {
                    // 需要重新計算世界變換
                    updateWorldTransform(world, e, transform);
                }
            }
        }

    private:
        static void updateWorldTransform(World& world, Entity e, TransformComponent& transform) {
            glm::mat4 localTransform = glm::translate(glm::mat4(1.0f), transform.position)
                                       * glm::mat4_cast(transform.rotation)
                                       * glm::scale(glm::mat4(1.0f), transform.scale);

            if (transform.parent != NULL_ENTITY) {
                auto* parentTransform = world.TryGetComponent<TransformComponent>(transform.parent);
                if (parentTransform) {
                    transform.worldTransform = parentTransform->worldTransform * localTransform;
                } else {
                    transform.worldTransform = localTransform;
                }
            } else {
                transform.worldTransform = localTransform;
            }

            transform.isDirty = false;
        }
    };

    // ============================================================================
    // 渲染系統 - 收集渲染實例
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
            World& world,
            std::vector<RenderInstance>& instances,
            std::unordered_map<std::shared_ptr<Material>, std::vector<std::shared_ptr<Mesh>>>& instanceBatches
        ) {
            auto* transformPool = world.GetPool<TransformComponent>();
            auto* renderPool = world.GetPool<MeshComponent>();

            for (size_t i = 0; i < renderPool->components.size(); ++i) {
                Entity e = renderPool->denseToEntity[i];
                auto& render = renderPool->components[i];

                if (!render.visible || !render.meshGroup) {
                    continue;
                }

                auto* transform = world.TryGetComponent<TransformComponent>(e);
                if (!transform) {
                    continue;
                }

                const glm::mat4& modelMatrix = transform->worldTransform;

                for (const auto& mesh : render.meshGroup->meshes) {
                    instances.push_back(
                        {
                            .model = modelMatrix,
                            .color = render.color,
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
    // 物理同步系統 - 同步 Transform 和 Physics
    // ============================================================================
    class PhysicsSyncSystem {
    public:
        // Scene → Physics: 同步 Kinematic/Static 物體
        static void syncToPhysics(World& world, Physics3D* physics) {
            auto* transformPool = world.GetPool<TransformComponent>();
            auto* physicsPool = world.GetPool<RigidbodyComponent>();

            for (size_t i = 0; i < physicsPool->components.size(); ++i) {
                Entity e = physicsPool->denseToEntity[i];
                auto& phys = physicsPool->components[i];

                if (!phys.body.valid() || !phys.syncToPhysics) {
                    continue;
                }

                auto* transform = world.TryGetComponent<TransformComponent>(e);
                if (!transform) {
                    continue;
                }

                // 檢查運動類型
                auto motionType = physics->getMotionType(phys.body);
                if (motionType == BodyMotionType::Kinematic || motionType == BodyMotionType::Static) {
                    physics->setPosition(phys.body, transform->position);
                    physics->setRotation(phys.body, transform->rotation);
                }
            }
        }

        // Physics → Scene: 同步 Dynamic 物體
        static void syncFromPhysics(World& world, Physics3D* physics) {
            auto* transformPool = world.GetPool<TransformComponent>();
            auto* physicsPool = world.GetPool<RigidbodyComponent>();

            for (size_t i = 0; i < physicsPool->components.size(); ++i) {
                Entity e = physicsPool->denseToEntity[i];
                auto& phys = physicsPool->components[i];

                if (!phys.body.valid() || !phys.syncFromPhysics) {
                    continue;
                }

                auto* transform = world.TryGetComponent<TransformComponent>(e);
                if (!transform) {
                    continue;
                }

                // 檢查運動類型
                auto motionType = physics->getMotionType(phys.body);
                if (motionType == BodyMotionType::Dynamic) {
                    transform->position = physics->getPosition(phys.body);
                    transform->rotation = physics->getRotation(phys.body);
                    transform->isDirty = true;// 標記需要重新計算世界變換
                }
            }
        }
    };

    // ============================================================================
    // 抓取系統 - 使用 ECS 設計
    // ============================================================================
    class GrabSystem {
    public:
        static bool
            tryPickup(World& world, Entity grabber, Physics3D* physics, Camera* camera, float pickupRange = 5.0f) {
            auto* grabberComp = world.TryGetComponent<GrabberComponent>(grabber);
            if (!grabberComp || grabberComp->heldEntity != NULL_ENTITY) {
                return false;// 已經抓著東西了
            }

            glm::vec3 rayStart = camera->getEye();
            glm::vec3 rayDir = camera->getForward();
            glm::vec3 rayEnd = rayStart + rayDir * pickupRange;

            RaycastHit hit;
            if (!physics->raycast(rayStart, rayEnd, hit)) {
                return false;
            }

            // 從 hit.node 找到對應的 Entity
            // 這需要一個 Node -> Entity 的映射（可以存儲在 Node 中）
            // 暫時假設可以通過某種方式找到 Entity

            // 檢查是否有 GrabbableComponent
            // Entity hitEntity = findEntityFromNode(hit.node);
            // if (!world.HasComponent<GrabbableComponent>(hitEntity)) {
            //     return false;
            // }

            // 添加 HeldComponent
            // auto& held = world.AddComponent<HeldComponent>(hitEntity);
            // held.holder = grabber;
            // held.originalGravityFactor = ...;

            // 更新 PhysicsComponent
            // auto* phys = world.TryGetComponent<PhysicsComponent>(hitEntity);
            // if (phys) {
            //     physics->setMotionType(phys->body, BodyMotionType::Kinematic);
            //     physics->setGravityFactor(phys->body, 0.0f);
            // }

            // grabberComp->heldEntity = hitEntity;

            return true;
        }

        static void update(World& world, Physics3D* physics, Camera* camera, float deltaTime) {
            auto* heldPool = world.GetPool<HeldComponent>();
            auto* transformPool = world.GetPool<TransformComponent>();

            for (size_t i = 0; i < heldPool->components.size(); ++i) {
                Entity e = heldPool->denseToEntity[i];
                auto& held = heldPool->components[i];

                if (held.holder == NULL_ENTITY) {
                    continue;
                }

                // 獲取抓取者的變換
                auto* holderTransform = world.TryGetComponent<TransformComponent>(held.holder);
                if (!holderTransform) {
                    continue;
                }

                // 計算目標位置（相機前方）
                glm::vec3 targetPos = camera->getEye() + camera->getForward() * held.holdDistance;

                // 更新物理體位置
                auto* phys = world.TryGetComponent<RigidbodyComponent>(e);
                if (phys && phys->body.valid()) {
                    glm::vec3 currentPos = physics->getPosition(phys->body);
                    glm::vec3 velocity = (targetPos - currentPos) / deltaTime;

                    float maxSpeed = 20.0f;
                    if (glm::length(velocity) > maxSpeed) {
                        velocity = glm::normalize(velocity) * maxSpeed;
                    }

                    physics->setLinearVelocity(phys->body, velocity);

                    // 同步到 Transform
                    auto* transform = world.TryGetComponent<TransformComponent>(e);
                    if (transform) {
                        transform->position = currentPos;
                        transform->isDirty = true;
                    }
                }
            }
        }
    };

    // ============================================================================
    // 光照移動系統 - Logic Driver
    // ============================================================================
    class LightMovementSystem {
    public:
        static void update(World& world, Scene* scene, float deltaTime) {
            auto* logicPool = world.GetPool<LightMovementLogicComponent>();
            auto* refPool = world.GetPool<SceneLightReferenceComponent>();

            for (size_t i = 0; i < logicPool->components.size(); ++i) {
                Entity e = logicPool->denseToEntity[i];
                auto& logic = logicPool->components[i];

                auto* ref = world.TryGetComponent<SceneLightReferenceComponent>(e);
                if (!ref || ref->lightIndex < 0 || ref->lightIndex >= scene->pointLights.size()) {
                    continue;
                }

                // Update timer
                logic.timer += deltaTime * logic.speed;
                float t = logic.timer;

                glm::vec3 newPos(0.0f);

                switch (logic.pattern) {
                case MovementPattern::Circle:
                    newPos.x = logic.radius * cos(t);
                    newPos.z = logic.radius * sin(t);
                    newPos.y = logic.height + 0.5f * sin(t * 0.5f);
                    break;

                case MovementPattern::Figure8:
                    newPos.x = (logic.radius + 1.0f) * sin(t * 0.7f);
                    newPos.z = (logic.radius + 1.0f) * sin(t * 0.7f) * cos(t * 0.7f);
                    newPos.y = 1.0f + 1.0f * cos(t * 0.3f);
                    break;

                case MovementPattern::Linear:
                    newPos.x = (logic.radius + 1.0f) * sin(t * 0.6f);
                    newPos.z = 2.0f * cos(t * 0.8f);
                    newPos.y = 0.5f + 2.0f * abs(sin(t * 0.4f));
                    break;

                case MovementPattern::Spiral: {
                    float spiralRadius = 2.0f + 1.0f * sin(t * 0.2f);
                    newPos.x = spiralRadius * cos(t * 0.5f);
                    newPos.z = spiralRadius * sin(t * 0.5f);
                    newPos.y = 0.5f + 2.5f * (1.0f - cos(t * 0.3f));
                    break;
                }
                }

                // Direct write to Scene
                scene->pointLights[ref->lightIndex].position = newPos;

                // Optional: Update intensity logic if needed (keeping it simple for now)
                scene->pointLights[ref->lightIndex].intensity = 3.0f + 2.0f * (0.5f + 0.5f * sin(t * 0.3f));
            }
        }
    };

    // ============================================================================
    // 相機系統
    // ============================================================================
    class CameraSystem {
    public:
        static void update(World& world, InputManager& inputManager, float deltaTime) {
            auto* cameraPool = world.GetPool<VirtualCameraComponent>();
            const auto& inputState = inputManager.getInputState();

            for (size_t i = 0; i < cameraPool->components.size(); ++i) {
                Entity e = cameraPool->denseToEntity[i];
                auto& cam = cameraPool->components[i];

                if (!cam.isActive) continue;

                // 1. Handle Fly Camera Logic
                if (auto* fly = world.TryGetComponent<FlyCameraComponent>(e)) {
                    handleFlyCamera(cam, fly, inputState, deltaTime);
                }

                // 2. Handle Follow Camera Logic
                if (auto* follow = world.TryGetComponent<FollowCameraComponent>(e)) {
                    handleFollowCamera(cam, follow, deltaTime);
                }

                // 3. Update Matrices
                updateMatrices(cam);
            }
        }

        static Entity getActiveCamera(World& world) {
            auto* cameraPool = world.GetPool<VirtualCameraComponent>();
            for (size_t i = 0; i < cameraPool->components.size(); ++i) {
                if (cameraPool->components[i].isActive) {
                    return cameraPool->denseToEntity[i];
                }
            }
            return NULL_ENTITY;
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
            // Simple Euler to Quat:
            cam.rotation = glm::quat(glm::vec3(glm::radians(-fly->pitch), glm::radians(fly->yaw - 90.0f), 0.0f));

            // Calculate Front/Right/Up vectors from rotation
            glm::vec3 front = cam.rotation * glm::vec3(0, 0, -1);// Assuming -Z is forward
            glm::vec3 right = cam.rotation * glm::vec3(1, 0, 0);
            glm::vec3 up = cam.rotation * glm::vec3(0, 1, 0);

            // Movement
            float speed = fly->moveSpeed * deltaTime;
            if (inputState.isPressed(InputAction::MoveForward)) cam.position += front * speed;
            if (inputState.isPressed(InputAction::MoveBackward)) cam.position -= front * speed;
            if (inputState.isPressed(InputAction::MoveLeft)) cam.position -= right * speed;
            if (inputState.isPressed(InputAction::MoveRight)) cam.position += right * speed;
            if (inputState.isPressed(InputAction::MoveUp)) cam.position += up * speed;
            if (inputState.isPressed(InputAction::MoveDown)) cam.position -= up * speed;
        }

        static void handleFollowCamera(VirtualCameraComponent& cam, FollowCameraComponent* follow, float deltaTime) {
            if (!follow->targetNode) return;

            // Simple follow logic: Target Position + Offset
            // We use the Node's world position directly
            glm::vec3 targetPos = follow->targetNode->getWorldPosition();
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

}// namespace Vapor
