#pragma once

#include <cmath>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

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

using EasingFunction = float (*)(float);

namespace Easing {
    inline float Linear(float t) {
        return t;
    }
    inline float InQuad(float t) {
        return t * t;
    }
    inline float OutQuad(float t) {
        return t * (2.0f - t);
    }
    inline float InOutQuad(float t) {
        return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    }
    inline float InCubic(float t) {
        return t * t * t;
    }
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
        if (t < 2.0f / 2.75f) {
            t -= 1.5f / 2.75f;
            return 7.5625f * t * t + 0.75f;
        }
        if (t < 2.5f / 2.75f) {
            t -= 2.25f / 2.75f;
            return 7.5625f * t * t + 0.9375f;
        }
        t -= 2.625f / 2.75f;
        return 7.5625f * t * t + 0.984375f;
    }
}// namespace Easing

// ============================================================
// Action Type
// ============================================================

enum class ActionType : uint8_t {
    // Tweens
    Position,// Move entity position
    Rotation,// Rotate entity
    Scale,// Scale entity
    Color,// Fade/color change
    Float,// Tween arbitrary float

    // Control
    Wait,// Wait for duration

    // Entity
    SetActive,// Show/hide entity
    PlayAnimation,// Play sprite/skeletal animation
};

// ============================================================
// Loop Mode
// ============================================================

enum class LoopMode : uint8_t {
    None,// Play once
    Loop,// Restart from beginning
    PingPong// Reverse direction
};

// ============================================================
// Action - Unified action structure
// ============================================================

struct ActionComponent {
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
    int loopCount = 1;// -1 = infinite
    int currentLoop = 0;
    bool pingPongReverse = false;

    // Target entity (null = use owner entity)
    entt::entity target = entt::null;

    // Values (used based on type)
    glm::vec3 vec3Start{ 0.0f };
    glm::vec3 vec3End{ 0.0f };
    glm::vec4 vec4Start{ 1.0f };
    glm::vec4 vec4End{ 1.0f };
    glm::quat quatStart{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::quat quatEnd{ 1.0f, 0.0f, 0.0f, 0.0f };
    float floatStart = 0.0f;
    float floatEnd = 0.0f;

    // For SetActive
    bool activeValue = true;

    // For PlayAnimation / debug
    std::string name;

    // Completion notification (0 = no event emitted)
    uint32_t completionTag = 0;

    // Helper: get progress (0-1)
    float getProgress() const {
        if (duration <= 0.0f) return 1.0f;
        float t = std::clamp(elapsed / duration, 0.0f, 1.0f);
        if (pingPongReverse) t = 1.0f - t;
        return easing ? easing(t) : t;
    }

    // Helper: check if action needs time
    bool isInstant() const {
        return type == ActionType::Callback || type == ActionType::SetActive || type == ActionType::PlayAnimation
               || duration <= 0.0f;
    }
};

// Event emitted when action/queue completes (1:N broadcast)
struct ActionCompleteEvent {
    uint32_t tag = 0;
};

// Parallel action group - tracks multiple simultaneous actions
struct ActionGroupComponent {
    uint32_t groupId = 0;
    size_t totalActions = 0;
    size_t completedActions = 0;
    uint32_t completionTag = 0; // Emitted when all complete

    bool isComplete() const { return completedActions >= totalActions; }
};

// Tag linking an action to a group
struct ActionGroupMemberTag {
    entt::entity groupEntity = entt::null;
};

// Queue of actions (sequential execution)
struct ActionQueueComponent {
    std::vector<ActionComponent> actions;
    size_t currentIndex = 0;
    uint32_t completionTag = 0; // 0 = no event emitted
    std::string debugName;

    bool isComplete() const {
        return currentIndex >= actions.size();
    }

    ActionComponent* current() {
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

    inline struct ActionComponent
        moveTo(entt::entity target, const glm::vec3& end, float duration, EasingFunction easing = Easing::OutCubic) {
        struct ActionComponent a;
        a.type = ActionType::Position;
        a.target = target;
        a.vec3End = end;
        a.duration = duration;
        a.easing = easing;
        return a;
    }

    inline struct ActionComponent
        moveBy(entt::entity target, const glm::vec3& delta, float duration, EasingFunction easing = Easing::OutCubic) {
        struct ActionComponent a;
        a.type = ActionType::Position;
        a.target = target;
        a.vec3End = delta;// Will be resolved to absolute in system
        a.duration = duration;
        a.easing = easing;
        a.name = "relative";// Flag for relative movement
        return a;
    }

    inline struct ActionComponent
        scaleTo(entt::entity target, const glm::vec3& end, float duration, EasingFunction easing = Easing::OutCubic) {
        struct ActionComponent a;
        a.type = ActionType::Scale;
        a.target = target;
        a.vec3End = end;
        a.duration = duration;
        a.easing = easing;
        return a;
    }

    inline struct ActionComponent
        rotateTo(entt::entity target, const glm::quat& end, float duration, EasingFunction easing = Easing::OutCubic) {
        struct ActionComponent a;
        a.type = ActionType::Rotation;
        a.target = target;
        a.quatEnd = end;
        a.duration = duration;
        a.easing = easing;
        return a;
    }

    inline struct ActionComponent
        fadeTo(entt::entity target, float alpha, float duration, EasingFunction easing = Easing::OutCubic) {
        struct ActionComponent a;
        a.type = ActionType::Color;
        a.target = target;
        a.vec4End = glm::vec4(1.0f, 1.0f, 1.0f, alpha);
        a.duration = duration;
        a.easing = easing;
        return a;
    }

    inline struct ActionComponent
        colorTo(entt::entity target, const glm::vec4& end, float duration, EasingFunction easing = Easing::OutCubic) {
        struct ActionComponent a;
        a.type = ActionType::Color;
        a.target = target;
        a.vec4End = end;
        a.duration = duration;
        a.easing = easing;
        return a;
    }

    // === Control ===

    inline struct ActionComponent wait(float duration) {
        struct ActionComponent a;
        a.type = ActionType::Wait;
        a.duration = duration;
        return a;
    }

    // Notify on completion (emits ActionCompleteEvent with this tag)
    inline struct ActionComponent& onComplete(struct ActionComponent& a, uint32_t tag) {
        a.completionTag = tag;
        return a;
    }

    // Create parallel action group - returns the group entity
    // Usage: auto group = Action::parallel(reg, { action1, action2 }, completionTag);
    inline entt::entity parallel(
        entt::registry& reg,
        std::vector<ActionComponent> actions,
        uint32_t completionTag = 0
    ) {
        static uint32_t nextGroupId = 1;

        auto groupEntity = reg.create();
        auto& group = reg.emplace<ActionGroupComponent>(groupEntity);
        group.groupId = nextGroupId++;
        group.totalActions = actions.size();
        group.completedActions = 0;
        group.completionTag = completionTag;

        for (auto& action : actions) {
            auto actionEntity = reg.create();
            reg.emplace<ActionComponent>(actionEntity, std::move(action));
            reg.emplace<ActionGroupMemberTag>(actionEntity, groupEntity);
        }

        return groupEntity;
    }

    // === Entity ===

    inline struct ActionComponent setActive(entt::entity target, bool active) {
        struct ActionComponent a;
        a.type = ActionType::SetActive;
        a.target = target;
        a.activeValue = active;
        return a;
    }

    inline struct ActionComponent playAnimation(entt::entity target, const std::string& animName) {
        struct ActionComponent a;
        a.type = ActionType::PlayAnimation;
        a.target = target;
        a.name = animName;
        return a;
    }

    // === Modifiers ===

    inline struct ActionComponent& loop(struct ActionComponent& a, int count = -1) {
        a.loopMode = LoopMode::Loop;
        a.loopCount = count;
        return a;
    }

    inline struct ActionComponent& pingPong(struct ActionComponent& a, int count = -1) {
        a.loopMode = LoopMode::PingPong;
        a.loopCount = count;
        return a;
    }

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

    inline ActionComponent move(
        const glm::vec3& from, const glm::vec3& to, float duration = 0.5f, EasingFunction easing = Easing::OutCubic
    ) {
        ActionComponent a;
        a.type = ActionType::Position;
        a.vec3Start = from;
        a.vec3End = to;
        a.duration = duration;
        a.easing = easing;
        return a;
    }

}// namespace Action
