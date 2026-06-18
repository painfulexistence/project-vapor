#pragma once
#include "Vapor/hidden.hpp"
#include "character_controller.hpp"
#include "graphics_handles.hpp"
#include "graphics_sprite.hpp"
#include "physics_3d.hpp"
#include "vehicle_controller.hpp"
#include <entt/entt.hpp>
#include <functional>
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
        glm::mat4    worldTransform = glm::mat4(1.0f);  // computed; mat4 skipped by inspector
        entt::entity parent = entt::null;               // shown read-only in inspector
        Hidden<bool> isDirty = {true};                  // internal bool hidden from inspector
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

    // ============================================================================
    // Character Controller (ECS-owned)
    // ============================================================================
    struct CharacterBodyComponent {
        CharacterControllerSettings settings;
        std::unique_ptr<CharacterController> controller;
        glm::vec3 desiredVelocity = glm::vec3(0.0f);
        bool jumpRequested = false;
    };

    // ============================================================================
    // Vehicle Controller (ECS-owned)
    // ============================================================================
    struct VehicleBodyComponent {
        VehicleSettings settings;
        std::unique_ptr<VehicleController> controller;
        float throttle  = 0.0f;
        float steering  = 0.0f;
        float brake     = 0.0f;
        bool  handbrake = false;
    };

    // ============================================================================
    // Trigger Volume (ECS-owned) - Pure data, no callbacks
    // ============================================================================
    struct TriggerVolumeComponent {
        TriggerHandle trigger;
    };

    // Event components emitted by TriggerSystem when entities enter/exit triggers
    struct TriggerEnterEvent {
        entt::entity triggerEntity;  // The trigger volume entity
        entt::entity otherEntity;    // The entity that entered
    };

    struct TriggerExitEvent {
        entt::entity triggerEntity;  // The trigger volume entity
        entt::entity otherEntity;    // The entity that exited
    };

    // 2D Sprite rendering component
    struct SpriteComponent {
        AtlasHandle atlas;
        uint16_t frameIndex = 0;

        glm::vec2 size = {1.0f, 1.0f};       // World units
        glm::vec2 pivot = {0.5f, 0.5f};      // Anchor point (0-1)
        glm::vec4 tint = {1, 1, 1, 1};       // Color tint
        int sortingLayer = 0;
        int orderInLayer = 0;
        bool flipX = false;
        bool flipY = false;
        bool visible = true;
    };

    // Flipbook animation component (drives any frame-based animation)
    struct FlipbookComponent {
        std::vector<uint16_t> frameIndices;
        float frameTime = 0.1f;              // Seconds per frame
        float timer = 0.0f;
        uint16_t currentIndex = 0;           // Index into frameIndices
        bool loop = true;
        bool playing = true;

        uint16_t getCurrentFrame() const {
            return frameIndices.empty() ? 0 : frameIndices[currentIndex];
        }
    };

}// namespace Vapor
