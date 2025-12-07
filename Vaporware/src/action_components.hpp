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
// Action        = pure data definition (can be stored, serialized, reused)
// ActionComponent = runtime state for a single action on an entity
// ActionSequence  = sequential execution of multiple actions on one entity
// ActionTimeline  = parallel execution across multiple entities
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
} // namespace Easing

// ============================================================
// Action Types
// ============================================================

enum class ActionType : uint8_t {
    MoveTo,
    MoveBy,
    ScaleTo,
    RotateTo,
    FadeTo,
    ColorTo,
    Wait,
    SetActive,
    PlayAnimation,
};

// ============================================================
// Action - Pure data definition
// ============================================================

struct Action {
    ActionType type = ActionType::Wait;
    float duration = 0.0f;
    EasingFunction easing = Easing::Linear;

    // Target entity (for actions triggered via Timeline)
    entt::entity target = entt::null;

    // Transform data
    glm::vec3 vec3Value{ 0.0f };
    glm::vec4 vec4Value{ 1.0f };
    glm::quat quatValue{ 1.0f, 0.0f, 0.0f, 0.0f };

    // Misc
    bool boolValue = true;
    std::string stringValue;

    // Completion callback tag (0 = no callback)
    uint32_t completionTag = 0;

    // === Fluent modifiers ===
    Action& dur(float d) { duration = d; return *this; }
    Action& ease(EasingFunction e) { easing = e; return *this; }
    Action& onComplete(uint32_t tag) { completionTag = tag; return *this; }

    // === Static factory methods ===

    static Action moveTo(const glm::vec3& pos) {
        Action a;
        a.type = ActionType::MoveTo;
        a.vec3Value = pos;
        return a;
    }

    static Action moveBy(const glm::vec3& delta) {
        Action a;
        a.type = ActionType::MoveBy;
        a.vec3Value = delta;
        return a;
    }

    static Action scaleTo(const glm::vec3& scale) {
        Action a;
        a.type = ActionType::ScaleTo;
        a.vec3Value = scale;
        return a;
    }

    static Action scaleTo(float uniform) {
        return scaleTo(glm::vec3(uniform));
    }

    static Action rotateTo(const glm::quat& rot) {
        Action a;
        a.type = ActionType::RotateTo;
        a.quatValue = rot;
        return a;
    }

    static Action fadeTo(float alpha) {
        Action a;
        a.type = ActionType::FadeTo;
        a.vec4Value = glm::vec4(1.0f, 1.0f, 1.0f, alpha);
        return a;
    }

    static Action colorTo(const glm::vec4& color) {
        Action a;
        a.type = ActionType::ColorTo;
        a.vec4Value = color;
        return a;
    }

    static Action wait(float duration) {
        Action a;
        a.type = ActionType::Wait;
        a.duration = duration;
        return a;
    }

    static Action setActive(bool active) {
        Action a;
        a.type = ActionType::SetActive;
        a.boolValue = active;
        return a;
    }

    static Action playAnimation(const std::string& name) {
        Action a;
        a.type = ActionType::PlayAnimation;
        a.stringValue = name;
        return a;
    }

    // === Presets ===

    static Action bounceIn(float duration = 0.5f) {
        return scaleTo(1.0f).dur(duration).ease(Easing::OutBack);
    }

    static Action bounceOut(float duration = 0.3f) {
        return scaleTo(0.0f).dur(duration).ease(Easing::InBack);
    }

    static Action fadeIn(float duration = 0.3f) {
        return fadeTo(1.0f).dur(duration).ease(Easing::OutCubic);
    }

    static Action fadeOut(float duration = 0.3f) {
        return fadeTo(0.0f).dur(duration).ease(Easing::OutCubic);
    }
};

// ============================================================
// ActionComponent - Runtime state for single action
// ============================================================

struct ActionComponent {
    Action action;

    // Runtime state
    float elapsed = 0.0f;
    bool started = false;

    // Captured start values (set when action starts)
    glm::vec3 startVec3{ 0.0f };
    glm::vec4 startVec4{ 1.0f };
    glm::quat startQuat{ 1.0f, 0.0f, 0.0f, 0.0f };

    ActionComponent() = default;
    ActionComponent(const Action& a) : action(a) {}
    ActionComponent(Action&& a) : action(std::move(a)) {}

    float getProgress() const {
        if (action.duration <= 0.0f) return 1.0f;
        float t = std::clamp(elapsed / action.duration, 0.0f, 1.0f);
        return action.easing ? action.easing(t) : t;
    }

    bool isComplete() const {
        return elapsed >= action.duration;
    }
};

// ============================================================
// ActionSequence - Sequential execution on one entity
// ============================================================

struct ActionSequence {
    std::vector<Action> actions;

    // Runtime state
    size_t currentIndex = 0;
    float elapsed = 0.0f;
    bool started = false;
    uint32_t completionTag = 0;

    // Captured start values for current action
    glm::vec3 startVec3{ 0.0f };
    glm::vec4 startVec4{ 1.0f };
    glm::quat startQuat{ 1.0f, 0.0f, 0.0f, 0.0f };

    ActionSequence() = default;
    ActionSequence(std::initializer_list<Action> list) : actions(list) {}

    bool isComplete() const { return currentIndex >= actions.size(); }

    Action* current() {
        return currentIndex < actions.size() ? &actions[currentIndex] : nullptr;
    }

    const Action* current() const {
        return currentIndex < actions.size() ? &actions[currentIndex] : nullptr;
    }

    void advance() {
        currentIndex++;
        elapsed = 0.0f;
        started = false;
    }

    float getProgress() const {
        auto* act = current();
        if (!act || act->duration <= 0.0f) return 1.0f;
        float t = std::clamp(elapsed / act->duration, 0.0f, 1.0f);
        return act->easing ? act->easing(t) : t;
    }

    // Builder pattern
    ActionSequence& then(Action action) {
        actions.push_back(std::move(action));
        return *this;
    }

    ActionSequence& onComplete(uint32_t tag) {
        completionTag = tag;
        return *this;
    }
};

// ============================================================
// ActionTimeline - Parallel execution across multiple entities
// ============================================================

struct ActionTimeline {
    struct Track {
        entt::entity target = entt::null;
        ActionSequence sequence;

        Track() = default;
        Track(entt::entity t, ActionSequence seq) : target(t), sequence(std::move(seq)) {}
        Track(entt::entity t, std::initializer_list<Action> actions) : target(t), sequence(actions) {}
    };

    std::vector<Track> tracks;
    uint32_t completionTag = 0;

    ActionTimeline() = default;
    ActionTimeline(std::initializer_list<Track> list) : tracks(list) {}

    bool isComplete() const {
        for (const auto& track : tracks) {
            if (!track.sequence.isComplete()) return false;
        }
        return true;
    }

    // Builder pattern
    ActionTimeline& track(entt::entity target, ActionSequence sequence) {
        tracks.push_back({ target, std::move(sequence) });
        return *this;
    }

    ActionTimeline& track(entt::entity target, std::initializer_list<Action> actions) {
        tracks.push_back({ target, actions });
        return *this;
    }

    ActionTimeline& onComplete(uint32_t tag) {
        completionTag = tag;
        return *this;
    }
};

// ============================================================
// ActionCompleteEvent - Emitted when action completes
// ============================================================

struct ActionCompleteEvent {
    uint32_t tag = 0;
};
