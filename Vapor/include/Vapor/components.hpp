#pragma once
#include "physics_3d.hpp"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Vapor {

    // ============================================================================
    // Core
    // ============================================================================
    struct NameComponent {
        std::string name;
    };

    struct TransformComponent {
        glm::vec3 position = glm::vec3(0.0f);
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale = glm::vec3(1.0f);
        glm::mat4 worldTransform = glm::mat4(1.0f);
        entt::entity parent = entt::null;
        bool isDirty = true;
    };

    struct Mesh;
    struct MeshRendererComponent {
        std::vector<std::shared_ptr<Mesh>> meshes;
        bool visible = true;
    };

    // ============================================================================
    // Physics
    // ============================================================================
    struct RigidbodyComponent {
        BodyHandle body;
        BodyMotionType motionType = BodyMotionType::Dynamic;
        bool syncToPhysics = false;
        bool syncFromPhysics = true;
    };

    struct BoxColliderComponent {
        glm::vec3 halfSize = glm::vec3(0.5f);
    };

    struct SphereColliderComponent {
        float radius = 0.5f;
    };

    // ============================================================================
    // Camera
    // ============================================================================
    struct VirtualCameraComponent {
        glm::vec3 position = glm::vec3(0.0f);
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        float fov = glm::radians(60.0f);
        float aspect = 1.0f;
        float near = 0.05f;
        float far = 500.0f;
        glm::mat4 viewMatrix = glm::mat4(1.0f);
        glm::mat4 projectionMatrix = glm::mat4(1.0f);
        bool isActive = false;
    };

    struct FlyCameraComponent {
        float moveSpeed = 5.0f;
        float rotateSpeed = 90.0f;
        float yaw = -90.0f;
        float pitch = 0.0f;
    };

    struct FollowCameraComponent {
        entt::entity target = entt::null;
        glm::vec3 offset = glm::vec3(0.0f, 2.0f, 5.0f);
        float smoothFactor = 0.1f;
        float deadzone = 0.1f;
    };

    // ============================================================================
    // Grab / Interaction
    // ============================================================================
    struct GrabberComponent {
        entt::entity heldEntity = entt::null;
        float maxPickupRange = 20.0f;
    };

    struct HeldByComponent {
        entt::entity holder = entt::null;
        float originalGravityFactor = 1.0f;
        float holdDistance = 3.0f;
    };

}// namespace Vapor
