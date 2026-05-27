#pragma once

#include "Vapor/components.hpp"
#include "Vapor/fsm.hpp"
#include <RmlUi/Core/ElementDocument.h>
#include <entt/entt.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// Use engine-layer components directly (avoid duplication)
// ============================================================================
using Vapor::FlyCameraComponent;
using Vapor::FollowCameraComponent;
using Vapor::GrabberComponent;
using Vapor::HeldByComponent;

// ============================================================================
// Scene References
// ============================================================================
struct ScenePointLightReferenceComponent {
    int lightIndex = -1;
};

struct SceneDirectionalLightReferenceComponent {
    int lightIndex = -1;
};

// ============================================================================
// Character Logic
// ============================================================================
struct CharacterIntent {
    glm::vec2 lookVector = glm::vec2(0.0f);
    glm::vec2 moveVector = glm::vec2(0.0f);
    float moveVerticalAxis = 0.0f;
    bool jump = false;
    bool sprint = false;
    bool interact = false;
};

struct CharacterControllerComponent {
    float moveSpeed = 5.0f;
    float rotateSpeed = 90.0f;
};

// ============================================================================
// Grabbable (game-specific, extends engine GrabberComponent pattern)
// ============================================================================
struct GrabbableComponent {
    float pickupRange = 5.0f;
    float holdOffset = 3.0f;
    float throwForce = 500.0f;
    bool isHeld = false;
};

// ============================================================================
// Light Logic
// ============================================================================
enum class MovementPattern { Circle, Figure8, Linear, Spiral };

struct LightMovementLogicComponent {
    MovementPattern pattern = MovementPattern::Circle;
    float speed = 1.0f;
    float timer = 0.0f;
    float radius = 3.0f;
    float height = 1.5f;
    float parameter1 = 0.0f;
    float parameter2 = 0.0f;
};

// ============================================================================
// Camera Logic (game-specific additions)
// ============================================================================
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

// ============================================================================
// Subtitle System (uses FSM)
// ============================================================================
namespace SubtitleStates {
    constexpr uint32_t Idle = 0;
    constexpr uint32_t WaitingForVisible = 1;
    constexpr uint32_t Displaying = 2;
    constexpr uint32_t WaitingForHidden = 3;
}

struct SubtitleEntry {
    std::string speaker;
    std::string text;
    float duration = 3.0f;
};

struct SubtitleQueueComponent {
    std::vector<SubtitleEntry> queue;
    int currentIndex = -1;
    bool advanceRequested = false;
    bool autoAdvance = true;
    float displayTimer = 0.0f;
};

inline Vapor::FSMDefinition createSubtitleFSM() {
    return Vapor::FSMDefinitionBuilder()
        .state("Idle")
        .state("WaitingForVisible")
        .state("Displaying")
        .state("WaitingForHidden")
        .transition("Idle", "WaitingForVisible", "ShowSubtitle")
        .transition("WaitingForVisible", "Displaying", "PageVisible")
        .transition("Displaying", "WaitingForHidden", "HideSubtitle")
        .transition("WaitingForHidden", "Idle", "PageHidden")
        .transition("WaitingForHidden", "WaitingForVisible", "ShowSubtitle")
        .initialState("Idle")
        .build();
}

// ============================================================================
// Chapter Title System (uses FSM)
// ============================================================================
namespace ChapterTitleStates {
    constexpr uint32_t Hidden = 0;
    constexpr uint32_t FadingIn = 1;
    constexpr uint32_t Visible = 2;
    constexpr uint32_t FadingOut = 3;
}

struct ChapterTitleComponent {
    std::string documentPath;
    Rml::ElementDocument* document = nullptr;
    std::string chapterNumber;
    std::string chapterTitle;
    bool showRequested = false;
    float fadeDuration = 0.8f;
    float displayDuration = 2.5f;
};

struct ChapterTitleTriggerComponent {
    std::string number;
    std::string title;
    bool showRequested = false;
};

inline Vapor::FSMDefinition createChapterTitleFSM() {
    return Vapor::FSMDefinitionBuilder()
        .state("Hidden")
        .state("FadingIn")
        .state("Visible")
        .state("FadingOut")
        .transition("Hidden", "FadingIn", "Show")
        .timedTransition("FadingIn", "Visible", 0.8f)
        .timedTransition("Visible", "FadingOut", 2.5f)
        .timedTransition("FadingOut", "Hidden", 0.8f)
        .initialState("Hidden")
        .build();
}

// ============================================================================
// Scene Transition System (uses FSM)
// ============================================================================
namespace SceneTransitionStates {
    constexpr uint32_t Idle = 0;
    constexpr uint32_t FadingInLoadingScreen = 1;
    constexpr uint32_t UnloadingScene = 2;
    constexpr uint32_t LoadingAssets = 3;
    constexpr uint32_t BuildingScene = 4;
    constexpr uint32_t FadingOutLoadingScreen = 5;
}

struct SceneTransitionComponent {
    std::string targetScene;
    float progress = 0.0f;
};

inline Vapor::FSMDefinition createSceneTransitionFSM() {
    return Vapor::FSMDefinitionBuilder()
        .state("Idle")
        .state("FadingInLoadingScreen")
        .state("UnloadingScene")
        .state("LoadingAssets")
        .state("BuildingScene")
        .state("FadingOutLoadingScreen")
        .transition("Idle", "FadingInLoadingScreen", "StartTransition")
        .transition("FadingInLoadingScreen", "UnloadingScene", "FadeInComplete")
        .transition("UnloadingScene", "LoadingAssets", "UnloadComplete")
        .transition("LoadingAssets", "BuildingScene", "AssetsLoaded")
        .transition("BuildingScene", "FadingOutLoadingScreen", "SceneBuilt")
        .transition("FadingOutLoadingScreen", "Idle", "FadeOutComplete")
        .initialState("Idle")
        .build();
}

// ============================================================================
// Scroll Text
// ============================================================================
struct ScrollTextQueueComponent {
    std::vector<std::string> lines;
    int currentIndex = 0;
    bool advanceRequested = false;
};

// ============================================================================
// Tags
// ============================================================================
struct PersistentTag {};
struct DeadTag {};