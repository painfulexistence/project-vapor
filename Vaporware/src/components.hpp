#pragma once

#include "Vapor/components.hpp"
#include "Vapor/scene.hpp"
#include <entt/entt.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
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

struct DeadTag {};


// ============================================================================
// Particle System Components
// ============================================================================

enum class EmitterShape {
    Point,   // 點發射
    Sphere,  // 球形發射（向所有方向）
    Cone,    // 錐形發射（帶角度）
    Box,     // 盒形區域發射
    Circle,  // 圓形平面發射
};

enum class EmitterMode {
    Continuous,  // 持續發射（根據 emissionRate）
    Burst,       // 週期性爆發
    Once,        // 發射一次後停止
};

struct ParticleEmitterComponent {
    // === 狀態控制 ===
    bool enabled = true;
    EmitterShape shape = EmitterShape::Cone;
    EmitterMode mode = EmitterMode::Continuous;

    // === 發射參數 ===
    float emissionRate = 10.0f;       // particles/second (Continuous mode)
    Uint32 burstCount = 50;           // particle count (Burst mode)
    float emitSpeed = 5.0f;           // 初始速度
    float emitSpeedVariation = 1.0f;  // 速度隨機變化範圍
    float emitAngle = 30.0f;          // 錐形角度 (degrees)
    glm::vec3 emitDirection = glm::vec3(0.0f, 1.0f, 0.0f);  // 發射方向

    // === 粒子生命週期 ===
    float particleLifetime = 2.0f;   // 粒子壽命（秒）
    float lifetimeVariation = 0.5f;  // 壽命隨機變化

    // === 外觀 ===
    float particleSize = 0.1f;
    float sizeVariation = 0.02f;
    glm::vec4 startColor = glm::vec4(1.0f);
    glm::vec4 endColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);  // 淡出

    // === 物理 ===
    glm::vec3 gravity = glm::vec3(0.0f, -9.8f, 0.0f);
    float damping = 0.1f;  // 速度衰減
    bool useAttractor = false;
    glm::vec3 attractorLocalPosition = glm::vec3(0.0f);  // 相對於 emitter
    float attractorStrength = 50.0f;

    // === 深度效果 ===
    bool depthFadeEnabled = false;    // 深度淡出（避免硬切割）
    float depthFadeDistance = 0.5f;   // 開始淡出的深度距離（世界單位）

    bool groundClampEnabled = false;  // 落葉貼地效果
    float groundOffset = 0.02f;       // 貼地時的偏移量
    float groundFriction = 0.8f;      // 落地後的摩擦力（0-1）

    // === 顏色 Palette（可選）===
    struct ColorPalette {
        glm::vec3 a = glm::vec3(0.5f);
        glm::vec3 b = glm::vec3(0.5f);
        glm::vec3 c = glm::vec3(1.0f);
        glm::vec3 d = glm::vec3(0.0f);
    } colorPalette;
    bool useColorPalette = false;

    // === Runtime State（由 ParticleSystem 管理，不要手動修改）===
    Uint32 maxParticles = 100;        // 此 emitter 最大粒子數
    Uint32 particleStartIndex = 0;    // 在全局 GPU buffer 中的起始索引
    Uint32 activeParticleCount = 0;   // 當前活躍粒子數
    float emissionTimer = 0.0f;       // 內部計時器
    Uint32 nextParticleIndex = 0;     // 下一個要發射的粒子索引（循環）
};

// 可選：Attractor 組件（可附加到任何 entity）
struct ParticleAttractorComponent {
    float strength = 50.0f;
    float radius = 10.0f;         // 影響範圍
    bool affectsAllEmitters = true;
};