#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <functional>
#include <variant>
#include <vector>
#include <string>
#include <cmath>
#include <optional>

// ============================================================
// Easing Functions (Data-Oriented, no dependency on engine layer)
// ============================================================

namespace Easing {
    inline float Linear(float t) { return t; }

    inline float InQuad(float t) { return t * t; }
    inline float OutQuad(float t) { return t * (2.0f - t); }
    inline float InOutQuad(float t) {
        return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    }

    inline float InCubic(float t) { return t * t * t; }
    inline float OutCubic(float t) { float f = t - 1.0f; return f * f * f + 1.0f; }
    inline float InOutCubic(float t) {
        if (t < 0.5f) return 4.0f * t * t * t;
        float f = (2.0f * t - 2.0f);
        return 0.5f * f * f * f + 1.0f;
    }

    inline float InQuart(float t) { return t * t * t * t; }
    inline float OutQuart(float t) { float f = t - 1.0f; return 1.0f - f * f * f * f; }

    inline float InExpo(float t) { return t == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * (t - 1.0f)); }
    inline float OutExpo(float t) { return t == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t); }

    inline float OutBack(float t) {
        const float c1 = 1.70158f;
        const float c3 = c1 + 1.0f;
        float f = t - 1.0f;
        return 1.0f + c3 * f * f * f + c1 * f * f;
    }

    inline float OutElastic(float t) {
        if (t == 0.0f || t == 1.0f) return t;
        const float p = 0.3f;
        return std::pow(2.0f, -10.0f * t) * std::sin((t - p / 4.0f) * (2.0f * 3.14159f) / p) + 1.0f;
    }

    inline float OutBounce(float t) {
        const float n1 = 7.5625f;
        const float d1 = 2.75f;
        if (t < 1.0f / d1) return n1 * t * t;
        if (t < 2.0f / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
        if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
        t -= 2.625f / d1;
        return n1 * t * t + 0.984375f;
    }
}

using EasingFunction = float(*)(float);

// ============================================================
// Tween Components - Data-Oriented Tweening
// ============================================================

enum class TweenState : uint8_t {
    Idle,
    Running,
    Paused,
    Completed
};

enum class TweenLoopMode : uint8_t {
    None,       // Play once
    Loop,       // Restart from beginning
    PingPong    // Reverse direction at end
};

// Base tween data - common to all tween types
struct TweenBase {
    float duration = 1.0f;
    float elapsed = 0.0f;
    float delay = 0.0f;
    EasingFunction easing = Easing::Linear;
    TweenState state = TweenState::Idle;
    TweenLoopMode loopMode = TweenLoopMode::None;
    int loopCount = -1;         // -1 = infinite, 0+ = remaining loops
    int currentLoop = 0;
    bool reverse = false;       // For PingPong mode
    std::string tag;            // Optional tag for grouping

    float getProgress() const {
        if (duration <= 0.0f) return 1.0f;
        float t = std::min(elapsed / duration, 1.0f);
        return reverse ? 1.0f - t : t;
    }

    float getEasedProgress() const {
        return easing ? easing(getProgress()) : getProgress();
    }
};

// Tween a float value
struct TweenFloatComponent {
    TweenBase base;
    float startValue = 0.0f;
    float endValue = 1.0f;
    float* target = nullptr;    // Direct pointer to target (optional)

    float getCurrentValue() const {
        return glm::mix(startValue, endValue, base.getEasedProgress());
    }
};

// Tween a Vec3 (position, scale, etc.)
struct TweenVec3Component {
    TweenBase base;
    glm::vec3 startValue{0.0f};
    glm::vec3 endValue{0.0f};

    glm::vec3 getCurrentValue() const {
        return glm::mix(startValue, endValue, base.getEasedProgress());
    }
};

// Tween a Quaternion (rotation)
struct TweenQuatComponent {
    TweenBase base;
    glm::quat startValue{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat endValue{1.0f, 0.0f, 0.0f, 0.0f};

    glm::quat getCurrentValue() const {
        return glm::slerp(startValue, endValue, base.getEasedProgress());
    }
};

// Tween color (Vec4 for RGBA)
struct TweenColorComponent {
    TweenBase base;
    glm::vec4 startValue{1.0f};
    glm::vec4 endValue{1.0f};

    glm::vec4 getCurrentValue() const {
        return glm::mix(startValue, endValue, base.getEasedProgress());
    }
};

// Generic tween that applies to TransformComponent
enum class TweenTransformTarget : uint8_t {
    Position,
    Rotation,
    Scale
};

struct TweenTransformComponent {
    TweenBase base;
    TweenTransformTarget target = TweenTransformTarget::Position;
    glm::vec3 startPosition{0.0f};
    glm::vec3 endPosition{0.0f};
    glm::quat startRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat endRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 startScale{1.0f};
    glm::vec3 endScale{1.0f};
};

// ============================================================
// Sprite Animation Components
// ============================================================

struct SpriteFrame {
    int frameIndex = 0;         // Index into sprite sheet or atlas
    float duration = 0.1f;      // Duration of this frame in seconds
    glm::vec2 uvMin{0.0f};      // UV coordinates (optional)
    glm::vec2 uvMax{1.0f};
};

enum class AnimationPlayMode : uint8_t {
    Once,           // Play once and stop
    Loop,           // Loop forever
    PingPong,       // Play forward then backward
    ClampForever    // Play once and hold last frame
};

struct SpriteAnimationComponent {
    std::vector<SpriteFrame> frames;
    int currentFrameIndex = 0;
    float frameTimer = 0.0f;
    float playbackSpeed = 1.0f;
    AnimationPlayMode playMode = AnimationPlayMode::Loop;
    bool isPlaying = true;
    bool reverse = false;       // For PingPong mode
    std::string currentAnimation;   // Name of current animation

    // Callbacks
    std::function<void()> onComplete;
    std::function<void(int)> onFrameChange;

    const SpriteFrame* getCurrentFrame() const {
        if (frames.empty() || currentFrameIndex < 0 ||
            currentFrameIndex >= static_cast<int>(frames.size())) {
            return nullptr;
        }
        return &frames[currentFrameIndex];
    }
};

// Animation database for named animations
struct SpriteAnimationClip {
    std::string name;
    std::vector<SpriteFrame> frames;
    AnimationPlayMode defaultPlayMode = AnimationPlayMode::Loop;
};

struct SpriteAnimatorComponent {
    std::vector<SpriteAnimationClip> clips;
    std::string currentClipName;
    int currentFrameIndex = 0;
    float frameTimer = 0.0f;
    float playbackSpeed = 1.0f;
    bool isPlaying = true;
    bool reverse = false;

    std::function<void(const std::string&)> onClipComplete;
    std::function<void(int)> onFrameChange;

    const SpriteAnimationClip* getCurrentClip() const {
        for (const auto& clip : clips) {
            if (clip.name == currentClipName) return &clip;
        }
        return nullptr;
    }

    const SpriteFrame* getCurrentFrame() const {
        const auto* clip = getCurrentClip();
        if (!clip || clip->frames.empty()) return nullptr;
        if (currentFrameIndex < 0 || currentFrameIndex >= static_cast<int>(clip->frames.size())) {
            return nullptr;
        }
        return &clip->frames[currentFrameIndex];
    }

    void play(const std::string& clipName, bool restart = false) {
        if (currentClipName != clipName || restart) {
            currentClipName = clipName;
            currentFrameIndex = 0;
            frameTimer = 0.0f;
            reverse = false;
        }
        isPlaying = true;
    }
};

// ============================================================
// Timeline / Cutscene Components
// ============================================================

// Actions that can be performed in a timeline
enum class TimelineActionType : uint8_t {
    Wait,               // Wait for duration
    MoveTo,             // Move entity to position
    RotateTo,           // Rotate entity to rotation
    ScaleTo,            // Scale entity
    FadeIn,             // Fade in (opacity 0 -> 1)
    FadeOut,            // Fade out (opacity 1 -> 0)
    PlayAnimation,      // Play sprite animation
    SetActive,          // Enable/disable entity
    Callback,           // Execute custom callback
    CameraLookAt,       // Camera look at position
    CameraMoveTo,       // Move camera to position
    Parallel,           // Execute multiple actions in parallel
    Sequence            // Execute multiple actions in sequence (nested timeline)
};

struct TimelineAction {
    TimelineActionType type = TimelineActionType::Wait;
    float duration = 1.0f;
    float elapsed = 0.0f;
    EasingFunction easing = Easing::Linear;
    bool started = false;
    bool completed = false;

    // Target entity (for entity-specific actions)
    entt::entity targetEntity = entt::null;

    // Data for different action types
    glm::vec3 vec3Start{0.0f};
    glm::vec3 vec3End{0.0f};
    glm::quat quatStart{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat quatEnd{1.0f, 0.0f, 0.0f, 0.0f};
    float floatStart = 0.0f;
    float floatEnd = 1.0f;
    std::string stringData;     // Animation name, etc.
    bool boolData = false;      // For SetActive
    std::function<void()> callback;

    // For Parallel/Sequence actions
    std::vector<TimelineAction> children;
};

struct TimelineTrack {
    std::string name;
    std::vector<TimelineAction> actions;
    size_t currentActionIndex = 0;
    bool isComplete = false;

    TimelineAction* getCurrentAction() {
        if (currentActionIndex >= actions.size()) return nullptr;
        return &actions[currentActionIndex];
    }
};

enum class TimelineState : uint8_t {
    Idle,
    Playing,
    Paused,
    Completed
};

struct TimelineComponent {
    std::vector<TimelineTrack> tracks;  // Multiple tracks for parallel execution
    float currentTime = 0.0f;
    float totalDuration = 0.0f;         // Calculated from tracks
    float playbackSpeed = 1.0f;
    TimelineState state = TimelineState::Idle;
    bool autoDestroy = false;           // Remove component when complete
    std::string tag;

    std::function<void()> onStart;
    std::function<void()> onComplete;
    std::function<void(float)> onUpdate;    // Called with normalized progress

    void play() { state = TimelineState::Playing; }
    void pause() { if (state == TimelineState::Playing) state = TimelineState::Paused; }
    void resume() { if (state == TimelineState::Paused) state = TimelineState::Playing; }
    void stop() { state = TimelineState::Idle; currentTime = 0.0f; }

    bool isPlaying() const { return state == TimelineState::Playing; }
    bool isComplete() const { return state == TimelineState::Completed; }

    float getProgress() const {
        return totalDuration > 0.0f ? currentTime / totalDuration : 0.0f;
    }
};

// Linear action sequence (used by FSM, triggers, or any action queue)
struct ActionQueueComponent {
    std::vector<TimelineAction> actions;
    size_t currentActionIndex = 0;
    TimelineState state = TimelineState::Idle;
    float playbackSpeed = 1.0f;
    bool autoDestroy = false;
    std::string tag;

    std::function<void()> onStart;
    std::function<void()> onComplete;

    void play() { state = TimelineState::Playing; }
    void pause() { if (state == TimelineState::Playing) state = TimelineState::Paused; }
    void stop() { state = TimelineState::Idle; currentActionIndex = 0; }

    TimelineAction* getCurrentAction() {
        if (currentActionIndex >= actions.size()) return nullptr;
        return &actions[currentActionIndex];
    }

    bool isComplete() const { return currentActionIndex >= actions.size(); }
};

// ============================================================
// Builder Helpers - Fluent API for creating animations
// ============================================================

namespace AnimationBuilder {

    // Create a simple position tween
    inline TweenTransformComponent moveFromTo(
        const glm::vec3& from,
        const glm::vec3& to,
        float duration,
        EasingFunction easing = Easing::OutCubic
    ) {
        TweenTransformComponent tween;
        tween.base.duration = duration;
        tween.base.easing = easing;
        tween.base.state = TweenState::Running;
        tween.target = TweenTransformTarget::Position;
        tween.startPosition = from;
        tween.endPosition = to;
        return tween;
    }

    // Create a rotation tween
    inline TweenTransformComponent rotateFromTo(
        const glm::quat& from,
        const glm::quat& to,
        float duration,
        EasingFunction easing = Easing::OutCubic
    ) {
        TweenTransformComponent tween;
        tween.base.duration = duration;
        tween.base.easing = easing;
        tween.base.state = TweenState::Running;
        tween.target = TweenTransformTarget::Rotation;
        tween.startRotation = from;
        tween.endRotation = to;
        return tween;
    }

    // Create a scale tween
    inline TweenTransformComponent scaleFromTo(
        const glm::vec3& from,
        const glm::vec3& to,
        float duration,
        EasingFunction easing = Easing::OutCubic
    ) {
        TweenTransformComponent tween;
        tween.base.duration = duration;
        tween.base.easing = easing;
        tween.base.state = TweenState::Running;
        tween.target = TweenTransformTarget::Scale;
        tween.startScale = from;
        tween.endScale = to;
        return tween;
    }

    // Create a color fade
    inline TweenColorComponent fadeFromTo(
        const glm::vec4& from,
        const glm::vec4& to,
        float duration,
        EasingFunction easing = Easing::Linear
    ) {
        TweenColorComponent tween;
        tween.base.duration = duration;
        tween.base.easing = easing;
        tween.base.state = TweenState::Running;
        tween.startValue = from;
        tween.endValue = to;
        return tween;
    }

    // Cutscene action builders
    inline TimelineAction wait(float duration) {
        TimelineAction action;
        action.type = TimelineActionType::Wait;
        action.duration = duration;
        return action;
    }

    inline TimelineAction moveTo(entt::entity target, const glm::vec3& to, float duration, EasingFunction easing = Easing::OutCubic) {
        TimelineAction action;
        action.type = TimelineActionType::MoveTo;
        action.targetEntity = target;
        action.vec3End = to;
        action.duration = duration;
        action.easing = easing;
        return action;
    }

    inline TimelineAction rotateTo(entt::entity target, const glm::quat& to, float duration, EasingFunction easing = Easing::OutCubic) {
        TimelineAction action;
        action.type = TimelineActionType::RotateTo;
        action.targetEntity = target;
        action.quatEnd = to;
        action.duration = duration;
        action.easing = easing;
        return action;
    }

    inline TimelineAction callback(std::function<void()> fn) {
        TimelineAction action;
        action.type = TimelineActionType::Callback;
        action.duration = 0.0f;
        action.callback = std::move(fn);
        return action;
    }

    inline TimelineAction playAnimation(entt::entity target, const std::string& animName) {
        TimelineAction action;
        action.type = TimelineActionType::PlayAnimation;
        action.targetEntity = target;
        action.stringData = animName;
        action.duration = 0.0f;  // Instant action
        return action;
    }

    inline TimelineAction setActive(entt::entity target, bool active) {
        TimelineAction action;
        action.type = TimelineActionType::SetActive;
        action.targetEntity = target;
        action.boolData = active;
        action.duration = 0.0f;
        return action;
    }

    inline TimelineAction parallel(std::initializer_list<TimelineAction> actions) {
        TimelineAction action;
        action.type = TimelineActionType::Parallel;
        action.children = actions;
        // Duration is max of all children
        for (const auto& child : action.children) {
            action.duration = std::max(action.duration, child.duration);
        }
        return action;
    }

    inline TimelineAction sequence(std::initializer_list<TimelineAction> actions) {
        TimelineAction action;
        action.type = TimelineActionType::Sequence;
        action.children = actions;
        // Duration is sum of all children
        for (const auto& child : action.children) {
            action.duration += child.duration;
        }
        return action;
    }

} // namespace AnimationBuilder

// ============================================================
// Tags for querying
// ============================================================

struct TweenActiveTag {};           // Entity has active tweens
struct AnimationActiveTag {};       // Entity has active sprite animations
struct TimelineActiveTag {};        // Entity has active timeline/cutscene
