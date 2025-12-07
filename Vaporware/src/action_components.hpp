#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <functional>
#include <vector>
#include <string>
#include <cmath>

// ============================================================
// Unified Action System
//
// Single concept for all time-based operations:
// - Tween (move, scale, fade, rotate)
// - Wait
// - Callback
// - Parallel execution
// - Animation playback
// ============================================================

// ============================================================
// Easing Functions
// ============================================================

using EasingFunction = float(*)(float);

namespace Easing {
    inline float Linear(float t) { return t; }
    inline float InQuad(float t) { return t * t; }
    inline float OutQuad(float t) { return t * (2.0f - t); }
    inline float InOutQuad(float t) { return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t; }
    inline float InCubic(float t) { return t * t * t; }
    inline float OutCubic(float t) { float f = t - 1.0f; return f * f * f + 1.0f; }
    inline float InOutCubic(float t) { return t < 0.5f ? 4.0f * t * t * t : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f; }
    inline float InBack(float t) { const float c = 1.70158f; return t * t * ((c + 1.0f) * t - c); }
    inline float OutBack(float t) { const float c = 1.70158f; float f = t - 1.0f; return f * f * ((c + 1.0f) * f + c) + 1.0f; }
    inline float InOutBack(float t) {
        const float c = 1.70158f * 1.525f;
        return t < 0.5f
            ? (2.0f * t) * (2.0f * t) * ((c + 1.0f) * 2.0f * t - c) / 2.0f
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
        t -= 2.625f / 2.75f; return 7.5625f * t * t + 0.984375f;
    }
}

// ============================================================
// Action Type
// ============================================================

enum class ActionType : uint8_t {
    // Tweens
    Position,       // Move entity position
    Rotation,       // Rotate entity
    Scale,          // Scale entity
    Color,          // Fade/color change
    Float,          // Tween arbitrary float

    // Control
    Wait,           // Wait for duration
    Callback,       // Execute function
    Parallel,       // Execute children in parallel

    // Entity
    SetActive,      // Show/hide entity
    PlayAnimation,  // Play sprite/skeletal animation
};

// ============================================================
// Loop Mode
// ============================================================

enum class LoopMode : uint8_t {
    None,           // Play once
    Loop,           // Restart from beginning
    PingPong        // Reverse direction
};

// ============================================================
// Action - Unified action structure
// ============================================================

struct Action {
    ActionType type = ActionType::Wait;

    // Timing
    float duration = 0.0f;
    float elapsed = 0.0f;
    EasingFunction easing = Easing::Linear;

    // State
    bool started = false;
    bool completed = false;

    // Loop
    LoopMode loopMode = LoopMode::None;
    int loopCount = 1;      // -1 = infinite
    int currentLoop = 0;
    bool pingPongReverse = false;

    // Target entity (null = use owner entity)
    entt::entity target = entt::null;

    // Values (used based on type)
    glm::vec3 vec3Start{0.0f};
    glm::vec3 vec3End{0.0f};
    glm::vec4 vec4Start{1.0f};
    glm::vec4 vec4End{1.0f};
    glm::quat quatStart{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat quatEnd{1.0f, 0.0f, 0.0f, 0.0f};
    float floatStart = 0.0f;
    float floatEnd = 0.0f;

    // For SetActive
    bool activeValue = true;

    // For PlayAnimation / debug
    std::string name;

    // For Callback
    std::function<void()> callback;

    // For Parallel
    std::vector<Action> children;

    // Helper: get progress (0-1)
    float getProgress() const {
        if (duration <= 0.0f) return 1.0f;
        float t = std::clamp(elapsed / duration, 0.0f, 1.0f);
        if (pingPongReverse) t = 1.0f - t;
        return easing ? easing(t) : t;
    }

    // Helper: check if action needs time
    bool isInstant() const {
        return type == ActionType::Callback ||
               type == ActionType::SetActive ||
               type == ActionType::PlayAnimation ||
               duration <= 0.0f;
    }
};

// ============================================================
// Action Components
// ============================================================

// Single action on an entity
struct ActionComponent : Action {};

// Queue of actions (sequential execution)
struct ActionQueueComponent {
    std::vector<Action> actions;
    size_t currentIndex = 0;
    std::function<void()> onComplete;
    std::string debugName;

    bool isComplete() const {
        return currentIndex >= actions.size();
    }

    Action* current() {
        if (currentIndex < actions.size()) {
            return &actions[currentIndex];
        }
        return nullptr;
    }

    void advance() {
        if (currentIndex < actions.size()) {
            currentIndex++;
        }
    }
};

// ============================================================
// Action Builder - Fluent API for creating actions
// ============================================================

namespace Action {

    // === Tweens ===

    inline struct Action moveTo(entt::entity target, const glm::vec3& end,
                                 float duration, EasingFunction easing = Easing::OutCubic) {
        struct Action a;
        a.type = ActionType::Position;
        a.target = target;
        a.vec3End = end;
        a.duration = duration;
        a.easing = easing;
        return a;
    }

    inline struct Action moveBy(entt::entity target, const glm::vec3& delta,
                                 float duration, EasingFunction easing = Easing::OutCubic) {
        struct Action a;
        a.type = ActionType::Position;
        a.target = target;
        a.vec3End = delta;  // Will be resolved to absolute in system
        a.duration = duration;
        a.easing = easing;
        a.name = "relative";  // Flag for relative movement
        return a;
    }

    inline struct Action scaleTo(entt::entity target, const glm::vec3& end,
                                  float duration, EasingFunction easing = Easing::OutCubic) {
        struct Action a;
        a.type = ActionType::Scale;
        a.target = target;
        a.vec3End = end;
        a.duration = duration;
        a.easing = easing;
        return a;
    }

    inline struct Action rotateTo(entt::entity target, const glm::quat& end,
                                   float duration, EasingFunction easing = Easing::OutCubic) {
        struct Action a;
        a.type = ActionType::Rotation;
        a.target = target;
        a.quatEnd = end;
        a.duration = duration;
        a.easing = easing;
        return a;
    }

    inline struct Action fadeTo(entt::entity target, float alpha,
                                 float duration, EasingFunction easing = Easing::OutCubic) {
        struct Action a;
        a.type = ActionType::Color;
        a.target = target;
        a.vec4End = glm::vec4(1.0f, 1.0f, 1.0f, alpha);
        a.duration = duration;
        a.easing = easing;
        return a;
    }

    inline struct Action colorTo(entt::entity target, const glm::vec4& end,
                                  float duration, EasingFunction easing = Easing::OutCubic) {
        struct Action a;
        a.type = ActionType::Color;
        a.target = target;
        a.vec4End = end;
        a.duration = duration;
        a.easing = easing;
        return a;
    }

    // === Control ===

    inline struct Action wait(float duration) {
        struct Action a;
        a.type = ActionType::Wait;
        a.duration = duration;
        return a;
    }

    inline struct Action call(std::function<void()> fn) {
        struct Action a;
        a.type = ActionType::Callback;
        a.callback = std::move(fn);
        return a;
    }

    inline struct Action parallel(std::vector<struct Action> children) {
        struct Action a;
        a.type = ActionType::Parallel;
        a.children = std::move(children);
        // Duration is max of children
        for (const auto& child : a.children) {
            a.duration = std::max(a.duration, child.duration);
        }
        return a;
    }

    // === Entity ===

    inline struct Action setActive(entt::entity target, bool active) {
        struct Action a;
        a.type = ActionType::SetActive;
        a.target = target;
        a.activeValue = active;
        return a;
    }

    inline struct Action playAnimation(entt::entity target, const std::string& animName) {
        struct Action a;
        a.type = ActionType::PlayAnimation;
        a.target = target;
        a.name = animName;
        return a;
    }

    // === Modifiers ===

    inline struct Action& loop(struct Action& a, int count = -1) {
        a.loopMode = LoopMode::Loop;
        a.loopCount = count;
        return a;
    }

    inline struct Action& pingPong(struct Action& a, int count = -1) {
        a.loopMode = LoopMode::PingPong;
        a.loopCount = count;
        return a;
    }

}  // namespace Action

// ============================================================
// Preset Tween Actions (returns ActionComponent data)
// ============================================================

namespace Tween {

    inline ActionComponent bounceIn(float duration = 0.5f) {
        ActionComponent a;
        a.type = ActionType::Scale;
        a.vec3Start = glm::vec3(0.0f);
        a.vec3End = glm::vec3(1.0f);
        a.duration = duration;
        a.easing = Easing::OutBack;
        return a;
    }

    inline ActionComponent bounceOut(float duration = 0.3f) {
        ActionComponent a;
        a.type = ActionType::Scale;
        a.vec3Start = glm::vec3(1.0f);
        a.vec3End = glm::vec3(0.0f);
        a.duration = duration;
        a.easing = Easing::InBack;
        return a;
    }

    inline ActionComponent pulse(float minScale = 0.9f, float maxScale = 1.1f, float duration = 1.0f) {
        ActionComponent a;
        a.type = ActionType::Scale;
        a.vec3Start = glm::vec3(minScale);
        a.vec3End = glm::vec3(maxScale);
        a.duration = duration;
        a.easing = Easing::InOutQuad;
        a.loopMode = LoopMode::PingPong;
        a.loopCount = -1;
        return a;
    }

    inline ActionComponent fadeOut(float duration = 0.3f) {
        ActionComponent a;
        a.type = ActionType::Color;
        a.vec4Start = glm::vec4(1.0f);
        a.vec4End = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        a.duration = duration;
        a.easing = Easing::OutCubic;
        return a;
    }

    inline ActionComponent fadeIn(float duration = 0.3f) {
        ActionComponent a;
        a.type = ActionType::Color;
        a.vec4Start = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
        a.vec4End = glm::vec4(1.0f);
        a.duration = duration;
        a.easing = Easing::OutCubic;
        return a;
    }

    inline ActionComponent move(const glm::vec3& from, const glm::vec3& to,
                                 float duration = 0.5f, EasingFunction easing = Easing::OutCubic) {
        ActionComponent a;
        a.type = ActionType::Position;
        a.vec3Start = from;
        a.vec3End = to;
        a.duration = duration;
        a.easing = easing;
        return a;
    }

}  // namespace Tween
