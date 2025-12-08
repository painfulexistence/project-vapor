#pragma once

#include "Vapor/components.hpp"
#include "Vapor/scene.hpp"
#include <entt/entt.hpp>
#include <glm/vec2.hpp>
#include <memory>

namespace Rml {
    class ElementDocument;
}

struct SceneNodeReferenceComponent {
    std::shared_ptr<Node> node = nullptr;
};

struct ScenePointLightReferenceComponent {
    int lightIndex = -1;// Index into Scene::pointLights
};

struct SceneDirectionalLightReferenceComponent {
    int lightIndex = -1;// Index into Scene::directionalLights
};

// Character Logic
struct CharacterIntent {
    glm::vec2 lookVector = glm::vec2(0.0f);
    glm::vec2 moveVector = glm::vec2(0.0f);
    float moveVerticalAxis = 0.0f;
    bool jump;
    bool sprint;
    bool interact;
};

struct CharacterControllerComponent {
    float moveSpeed = 5.0f;
    float rotateSpeed = 90.0f;
};


// Grabbable / Interaction
struct GrabbableComponent {
    float pickupRange = 5.0f;
    float holdOffset = 3.0f;
    float throwForce = 500.0f;
    bool isHeld = false;
};

struct HeldByComponent {
    entt::entity holder = entt::null;
    float originalGravityFactor = 1.0f;
    float holdDistance = 3.0f;
};

struct GrabberComponent {
    entt::entity heldEntity = entt::null;
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

    float yaw = -90.0f;
    float pitch = 0.0f;
};

struct FollowCameraComponent {
    entt::entity target = entt::null;

    glm::vec3 offset = glm::vec3(0.0f, 2.0f, 5.0f);
    float smoothFactor = 0.1f;
    float deadzone = 0.1f;
};

struct FirstPersonCameraComponent {
    float moveSpeed = 5.0f;
    float rotateSpeed = 90.0f;

    float yaw = -90.0f;
    float pitch = 0.0f;
};

struct CameraSwitchRequest {
    enum class Mode { Free, Follow, FirstPerson } mode;
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


// UI Components
enum class HUDState { Hidden, FadingIn, Visible, FadingOut };

struct HUDComponent {
    std::string documentPath;
    Rml::ElementDocument* document = nullptr;// Runtime only
    bool isVisible = false;

    // Transition support
    HUDState state = HUDState::Visible;
    float timer = 0.0f;
    float fadeDuration = 0.5f;
};

// Scroll Text (Teleprompter-style)
enum class ScrollTextState { Idle, ScrollingOut, PreparingScrollIn, ScrollingIn };

struct ScrollTextComponent {
    std::string documentPath;
    Rml::ElementDocument* document = nullptr;

    std::vector<std::string> lines;
    int currentIndex = 0;
    bool advanceRequested = false;

    // Animation state
    ScrollTextState state = ScrollTextState::Idle;
    float timer = 0.0f;
    float scrollDuration = 0.4f;
};

// Letterbox (Cinematic black bars)
enum class LetterboxState { Hidden, Opening, Open, Closing };

struct LetterboxComponent {
    std::string documentPath;
    Rml::ElementDocument* document = nullptr;

    bool isOpen = false;

    // Animation state
    LetterboxState state = LetterboxState::Hidden;
    float timer = 0.0f;
    float animDuration = 0.8f;

    // Target aspect ratio (2.35:1 = cinematic widescreen)
    float targetAspect = 2.35f;
};

struct DeadTag {};