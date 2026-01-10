#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace Vapor {

// ============================================================
// FSM Components - Pure Data
// ============================================================

/**
 * Core FSM state data.
 */
struct FSMStateComponent {
    uint32_t currentState = 0;
    float stateTime = 0.0f;
    float totalTime = 0.0f;
};

/**
 * State change event - emitted by FSMSystem when state changes.
 * Consumed by other systems to trigger effects.
 *
 * Pattern:
 *   FSMSystem emits → EffectsSystem consumes → Request components
 */
struct FSMStateChangeEvent {
    uint32_t fromState;
    uint32_t toState;
    float previousStateTime;  // How long we were in the previous state
};

/**
 * Transition rule - event triggered.
 */
struct FSMTransitionRule {
    uint32_t fromState;
    uint32_t toState;
    std::string triggerEvent;
    float minStateTime = 0.0f;

    FSMTransitionRule() = default;
    FSMTransitionRule(uint32_t from, uint32_t to, std::string event, float minTime = 0.0f)
        : fromState(from), toState(to), triggerEvent(std::move(event)), minStateTime(minTime) {}
};

/**
 * Timed transition - auto-transition after duration.
 */
struct FSMTimedTransition {
    uint32_t fromState;
    uint32_t toState;
    float duration;

    FSMTimedTransition() = default;
    FSMTimedTransition(uint32_t from, uint32_t to, float dur)
        : fromState(from), toState(to), duration(dur) {}
};

/**
 * FSM definition - shared state machine structure.
 */
struct FSMDefinition {
    std::vector<std::string> stateNames;
    std::vector<FSMTransitionRule> eventTransitions;
    std::vector<FSMTimedTransition> timedTransitions;
    uint32_t initialState = 0;

    uint32_t getStateIndex(const std::string& name) const {
        for (uint32_t i = 0; i < stateNames.size(); ++i) {
            if (stateNames[i] == name) return i;
        }
        return 0;
    }

    const std::string& getStateName(uint32_t index) const {
        static const std::string empty;
        return index < stateNames.size() ? stateNames[index] : empty;
    }
};

/**
 * Event queue - input events pending processing.
 */
struct FSMEventQueue {
    std::vector<std::string> events;

    void push(const std::string& event) { events.push_back(event); }
    void push(std::string&& event) { events.push_back(std::move(event)); }
    void clear() { events.clear(); }
    bool empty() const { return events.empty(); }
};

// ============================================================
// Common Request Components - Output from effect systems
// ============================================================

/**
 * Request to spawn particles.
 * Consumed by ParticleSpawnSystem.
 */
struct ParticleBurstRequest {
    std::string configName;
    uint32_t count = 10;
    // Optional: offset from entity position
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float offsetZ = 0.0f;
};

/**
 * Request to trigger squash/stretch effect.
 * Consumed by SquashInitSystem.
 */
struct SquashRequest {
    float scaleX = 1.0f;
    float scaleY = 1.0f;
};

/**
 * Request to add camera trauma (screen shake).
 * Consumed by CameraTraumaSystem.
 */
struct CameraTraumaRequest {
    float amount = 0.5f;
};

/**
 * Request to play a sound.
 * Consumed by AudioSystem.
 */
struct SoundRequest {
    std::string soundName;
    float volume = 1.0f;
    float pitch = 1.0f;
};

/**
 * Request to play an animation.
 * Consumed by AnimationSystem.
 */
struct AnimationRequest {
    std::string animationName;
    bool loop = false;
    float speed = 1.0f;
};

// ============================================================
// FSM Builder
// ============================================================

class FSMDefinitionBuilder {
public:
    FSMDefinitionBuilder& state(const std::string& name) {
        m_definition.stateNames.push_back(name);
        return *this;
    }

    FSMDefinitionBuilder& transition(const std::string& from, const std::string& to,
                                      const std::string& event, float minTime = 0.0f) {
        uint32_t fromIdx = getOrCreateState(from);
        uint32_t toIdx = getOrCreateState(to);
        m_definition.eventTransitions.emplace_back(fromIdx, toIdx, event, minTime);
        return *this;
    }

    FSMDefinitionBuilder& timedTransition(const std::string& from, const std::string& to, float duration) {
        uint32_t fromIdx = getOrCreateState(from);
        uint32_t toIdx = getOrCreateState(to);
        m_definition.timedTransitions.emplace_back(fromIdx, toIdx, duration);
        return *this;
    }

    FSMDefinitionBuilder& initialState(const std::string& name) {
        m_definition.initialState = getOrCreateState(name);
        return *this;
    }

    FSMDefinition build() { return std::move(m_definition); }

private:
    FSMDefinition m_definition;

    uint32_t getOrCreateState(const std::string& name) {
        for (uint32_t i = 0; i < m_definition.stateNames.size(); ++i) {
            if (m_definition.stateNames[i] == name) return i;
        }
        m_definition.stateNames.push_back(name);
        return static_cast<uint32_t>(m_definition.stateNames.size() - 1);
    }
};

// ============================================================
// Common Events
// ============================================================

namespace FSMEvents {
    constexpr const char* Start = "Start";
    constexpr const char* Stop = "Stop";
    constexpr const char* Jump = "Jump";
    constexpr const char* Land = "Land";
    constexpr const char* Attack = "Attack";
    constexpr const char* Hurt = "Hurt";
    constexpr const char* Die = "Die";
}

} // namespace Vapor
