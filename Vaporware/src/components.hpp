#pragma once

#include "Vapor/components.hpp"
#include "Vapor/scene.hpp"
#include <glm/glm.hpp>
#include <memory>

// Grabbable / Interaction
struct GrabbableComponent {
    float pickupRange = 5.0f;
    float holdOffset = 3.0f;
    float throwForce = 500.0f;
    bool isHeld = false;
};

struct HeldComponent {
    Vapor::Entity holder = Vapor::NULL_ENTITY;
    float originalGravityFactor = 1.0f;
    float holdDistance = 3.0f;
};

struct GrabberComponent {
    Vapor::Entity heldEntity = Vapor::NULL_ENTITY;
    float maxPickupRange = 5.0f;
};

// Light Logic
enum class MovementPattern { Circle, Figure8, Linear, Spiral };

struct LightMovementLogicComponent {
    MovementPattern pattern;
    float speed = 1.0f;
    float timer = 0.0f;

    // Parameters
    float radius = 3.0f;
    float height = 1.5f;
    float parameter1 = 0.0f;
    float parameter2 = 0.0f;
};

// Camera Logic
struct FlyCameraComponent {
    float moveSpeed = 5.0f;
    float rotateSpeed = 90.0f;

    // Euler angles for free look
    float yaw = -90.0f;
    float pitch = 0.0f;
};

struct FollowCameraComponent {
    std::shared_ptr<Node> targetNode = nullptr;

    glm::vec3 offset = glm::vec3(0.0f, 2.0f, 5.0f);
    float smoothFactor = 0.1f;
    float deadzone = 0.1f;
};

// General Logic
struct SceneNodeReferenceComponent {
    std::shared_ptr<Node> node = nullptr;
};

struct ScenePointLightReferenceComponent {
    int lightIndex = -1;// Index into Scene::pointLights
};

struct SceneDirectionalLightReferenceComponent {
    int lightIndex = -1;// Index into Scene::directionalLights
};

struct AutoRotateComponent {
    glm::vec3 axis = glm::vec3(0.0f, 1.0f, 0.0f);
    float speed = 1.0f;
};

struct DirectionalLightLogicComponent {
    glm::vec3 baseDirection = glm::vec3(0.5f, -1.0f, 0.0f);
    float speed = 1.0f;
    float magnitude = 0.05f;
    float timer = 0.0f;
};