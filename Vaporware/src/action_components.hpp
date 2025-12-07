#pragma once

#include <cmath>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

// ============================================================
// Action System
//
// Action = pure data definition (can be stored, serialized, reused)
// ActionComponent = ECS runtime state (attached to entity during execution)
// ============================================================

// ============================================================
// Easing Functions
// ============================================================

using EasingFunction = float (*)(float);

namespace Easing {
    inline float Linear(float t) { return t; }
    inline float InQuad(float t) { return t * t; }
    inline float OutQuad(float t) { return t * (2.0f - t); }
    inline float InOutQuad(float t) {
        return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    }
    inline float InCubic(float t) { return t * t * t; }
    inline float OutCubic(float t) {
        float f = t - 1.0f;
        return f * f * f + 1.0f;
    }
    inline float InOutCubic(float t) {
        return t < 0.5f ? 4.0f * t * t * t : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
    }
    inline float InBack(float t) {
        const float c = 1.70158f;
        return t * t * ((c + 1.0f) * t - c);
    }
    inline float OutBack(float t) {
        const float c = 1.70158f;
        float f = t - 1.0f;
        return f * f * ((c + 1.0f) * f + c) + 1.0f;
    }
    inline float InOutBack(float t) {
        const float c = 1.70158f * 1.525f;
        return t < 0.5f ? (2.0f * t) * (2.0f * t) * ((c + 1.0f) * 2.0f * t - c) / 2.0f
                        : ((2.0f * t - 2.0f) * (2.0f * t - 2.0f) * ((c + 1.0f) * (2.0f * t - 2.0f) + c) + 2.0f) / 2.0f;
    }
    inline float OutElastic(float t) {
        if (t == 0.0f || t == 1.0f) return t;
        return std::pow(2.0f, -10.0f * t) * std::sin((t - 0.075f) * (2.0f * 3.14159f) / 0.3f) + 1.0f;
    }
    inline float OutBounce(float t) {
        if (t < 1.0f / 2.75f) return 7.5625f * t * t;
        if (t < 2.0f / 2.75f) { t -= 1.5f / 2.75f; return 7.5625f * t * t + 0.75f; }
        if (t < 2.5f / 2.75f) { t -= 2.25f / 2.75f; return 7.5625f * t * t + 0.9375f; }
        t -= 2.625f / 2.75f;
        return 7.5625f * t * t + 0.984375f;
    }
}// namespace Easing

// ============================================================
// Enums
// ============================================================

enum class ActionType : uint8_t {
    Position,      // Move entity position
    Rotation,      // Rotate entity
    Scale,         // Scale entity
    Color,         // Fade/color change
    Float,         // Tween arbitrary float
    Wait,          // Wait for duration
    SetActive,     // Show/hide entity
    PlayAnimation, // Play sprite/skeletal animation
};

enum class LoopMode : uint8_t {
    None,    // Play once
    Loop,    // Restart from beginning
    PingPong // Reverse direction
};

// ============================================================
// Action - Pure data definition (POD with static factory methods)
// ============================================================

struct Action {
    // === Data ===
    ActionType type = ActionType::Wait;
    float duration = 0.0f;
    EasingFunction easing = Easing::Linear;

    LoopMode loopMode = LoopMode::None;
    int loopCount = 1; // -1 = infinite

    entt::entity target = entt::null;

    glm::vec3 vec3Start{ 0.0f };
    glm::vec3 vec3End{ 0.0f };
    glm::vec4 vec4Start{ 1.0f };
    glm::vec4 vec4End{ 1.0f };
    glm::quat quatStart{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::quat quatEnd{ 1.0f, 0.0f, 0.0f, 0.0f };
    float floatStart = 0.0f;
    float floatEnd = 0.0f;

    bool activeValue = true;
    std::string name;

    uint32_t completionTag = 0;

    // === Fluent modifiers ===
    Action& dur(float d) { duration = d; return *this; }
    Action& ease(EasingFunction e) { easing = e; return *this; }
    Action& onComplete(uint32_t tag) { completionTag = tag; return *this; }
    Action& loop(int count = -1) { loopMode = LoopMode::Loop; loopCount = count; return *this; }
    Action& pingPong(int count = -1) { loopMode = LoopMode::PingPong; loopCount = count; return *this; }

    // === Helpers ===
    bool isInstant() const {
        return type == ActionType::SetActive || type == ActionType::PlayAnimation || duration <= 0.0f;
    }

    // === Static factory methods ===

    static Action moveTo(entt::entity target, const glm::vec3& end) {
        Action a;
        a.type = ActionType::Position;
        a.target = target;
        a.vec3End = end;
        return a;
    }

    static Action moveBy(entt::entity target, const glm::vec3& delta) {
        Action a;
        a.type = ActionType::Position;
        a.target = target;
        a.vec3End = delta;
        a.name = "relative";
        return a;
    }

    static Action scaleTo(entt::entity target, const glm::vec3& end) {
        Action a;
        a.type = ActionType::Scale;
        a.target = target;
        a.vec3End = end;
        return a;
    }

    static Action rotateTo(entt::entity target, const glm::quat& end) {
        Action a;
        a.type = ActionType::Rotation;
        a.target = target;
        a.quatEnd = end;
        return a;
    }

    static Action fadeTo(entt::entity target, float alpha) {
        Action a;
        a.type = ActionType::Color;
        a.target = target;
        a.vec4End = glm::vec4(1.0f, 1.0f, 1.0f, alpha);
        return a;
    }

    static Action colorTo(entt::entity target, const glm::vec4& end) {
        Action a;
        a.type = ActionType::Color;
        a.target = target;
        a.vec4End = end;
        return a;
    }

    static Action wait(float duration) {
        Action a;
        a.type = ActionType::Wait;
        a.duration = duration;
        return a;
    }

    static Action setActive(entt::entity target, bool active) {
        Action a;
        a.type = ActionType::SetActive;
        a.target = target;
        a.activeValue = active;
        return a;
    }

    static Action playAnimation(entt::entity target, const std::string& animName) {
        Action a;
        a.type = ActionType::PlayAnimation;
        a.target = target;
        a.name = animName;
        return a;
    }

    // === Presets ===

    static Action bounceIn() {
        Action a;
        a.type = ActionType::Scale;
        a.vec3Start = glm::vec3(0.0f);
        a.vec3End = glm::vec3(1.0f);
        a.duration = 0.5f;
        a.easing = Easing::OutBack;
        return a;
    }

    static Action bounceOut() {
        Action a;
        a.type = ActionType::Scale;
        a.vec3Start = glm::vec3(1.0f);
        a.vec3End = glm::vec3(0.0f);
        a.duration = 0.3f;
        a.easing = Easing::InBack;
        return a;
    }

    static Action pulse(float minScale = 0.9f, float maxScale = 1.1f) {
        Action a;
        a.type = ActionType::Scale;
        a.vec3Start = glm::vec3(minScale);
        a.vec3End = glm::vec3(maxScale);
        a.duration = 1.0f;
        a.easing = Easing::InOutQuad;
        a.loopMode = LoopMode::PingPong;
        a.loopCount = -1;
        return a;
    }

    static Action fadeOut() {
        Action a;
        a.type = ActionType::Color;
        a.vec4Start = glm::vec4(1.0f);
        a.vec4End = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        a.duration = 0.3f;
        a.easing = Easing::OutCubic;
        return a;
    }

    static Action fadeIn() {
        Action a;
        a.type = ActionType::Color;
        a.vec4Start = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        a.vec4End = glm::vec4(1.0f);
        a.duration = 0.3f;
        a.easing = Easing::OutCubic;
        return a;
    }
};

// ============================================================
// ActionComponent - ECS runtime state
// ============================================================

struct ActionComponent {
    Action action;

    // Runtime state
    float elapsed = 0.0f;
    bool started = false;
    bool completed = false;
    int currentLoop = 0;
    bool pingPongReverse = false;

    // Construct from Action
    ActionComponent() = default;
    ActionComponent(const Action& a) : action(a) {}
    ActionComponent(Action&& a) : action(std::move(a)) {}

    // Helper: get progress (0-1)
    float getProgress() const {
        if (action.duration <= 0.0f) return 1.0f;
        float t = std::clamp(elapsed / action.duration, 0.0f, 1.0f);
        if (pingPongReverse) t = 1.0f - t;
        return action.easing ? action.easing(t) : t;
    }

    bool isInstant() const { return action.isInstant(); }
};

// ============================================================
// Event & Group Components
// ============================================================

struct ActionCompleteEvent {
    uint32_t tag = 0;
};

struct ActionGroupComponent {
    uint32_t groupId = 0;
    size_t totalActions = 0;
    size_t completedActions = 0;
    uint32_t completionTag = 0;

    bool isComplete() const { return completedActions >= totalActions; }
};

struct ActionGroupMemberTag {
    entt::entity groupEntity = entt::null;
};

// ============================================================
// ActionQueueComponent - Sequential execution
// ============================================================

struct ActionQueueComponent {
    std::vector<Action> actions;
    size_t currentIndex = 0;
    uint32_t completionTag = 0;
    std::string debugName;

    // Runtime state for current action
    float elapsed = 0.0f;
    bool started = false;

    bool isComplete() const { return currentIndex >= actions.size(); }

    Action* current() {
        if (currentIndex < actions.size()) {
            return &actions[currentIndex];
        }
        return nullptr;
    }

    void advance() {
        if (currentIndex < actions.size()) {
            currentIndex++;
            elapsed = 0.0f;
            started = false;
        }
    }
};
