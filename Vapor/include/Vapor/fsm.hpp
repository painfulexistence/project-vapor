#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace Vapor {

// ============================================================
// FSM Components - Pure Data, No Callbacks
// ============================================================

/**
 * Core FSM state component - pure data.
 *
 * Systems can query justEntered/justExited to react to state changes
 * instead of using callbacks.
 */
struct FSMStateComponent {
    uint32_t currentState = 0;
    uint32_t previousState = 0;
    float stateTime = 0.0f;       // Time spent in current state
    float totalTime = 0.0f;       // Total FSM runtime
    bool justEntered = false;     // True on the frame we entered current state
    bool justExited = false;      // True on the frame we left previous state

    // Check if we just transitioned this frame
    bool hasTransitioned() const { return justEntered; }

    // Check if in a specific state (by index)
    bool isInState(uint32_t stateIndex) const { return currentState == stateIndex; }
};

/**
 * Transition rule - pure data definition.
 */
struct FSMTransitionRule {
    uint32_t fromState;
    uint32_t toState;
    std::string triggerEvent;     // Event name that triggers this transition
    float minStateTime = 0.0f;    // Minimum time in state before transition allowed

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
 * FSM definition component - defines states and transitions.
 * Shared across entities with the same FSM structure.
 */
struct FSMDefinition {
    std::vector<std::string> stateNames;
    std::vector<FSMTransitionRule> eventTransitions;
    std::vector<FSMTimedTransition> timedTransitions;
    uint32_t initialState = 0;

    // Helper to get state index by name
    uint32_t getStateIndex(const std::string& name) const {
        for (uint32_t i = 0; i < stateNames.size(); ++i) {
            if (stateNames[i] == name) return i;
        }
        return 0;
    }

    // Helper to get state name by index
    const std::string& getStateName(uint32_t index) const {
        static const std::string empty;
        return index < stateNames.size() ? stateNames[index] : empty;
    }

    // Check if state index is valid
    bool isValidState(uint32_t index) const {
        return index < stateNames.size();
    }
};

/**
 * Event queue component - holds pending events.
 */
struct FSMEventQueue {
    std::vector<std::string> events;

    void push(const std::string& event) {
        events.push_back(event);
    }

    void push(std::string&& event) {
        events.push_back(std::move(event));
    }

    void clear() {
        events.clear();
    }

    bool empty() const {
        return events.empty();
    }
};

/**
 * Optional: FSM variables for storing state-machine-scoped data.
 * Use typed components instead when possible for better ECS patterns.
 */
struct FSMVariables {
    std::unordered_map<std::string, float> floats;
    std::unordered_map<std::string, int> ints;
    std::unordered_map<std::string, bool> bools;

    void setFloat(const std::string& name, float value) { floats[name] = value; }
    void setInt(const std::string& name, int value) { ints[name] = value; }
    void setBool(const std::string& name, bool value) { bools[name] = value; }

    float getFloat(const std::string& name, float defaultVal = 0.0f) const {
        auto it = floats.find(name);
        return it != floats.end() ? it->second : defaultVal;
    }

    int getInt(const std::string& name, int defaultVal = 0) const {
        auto it = ints.find(name);
        return it != ints.end() ? it->second : defaultVal;
    }

    bool getBool(const std::string& name, bool defaultVal = false) const {
        auto it = bools.find(name);
        return it != bools.end() ? it->second : defaultVal;
    }
};

// ============================================================
// FSM Builder - Fluent API for building FSMDefinition
// ============================================================

/**
 * Builder for creating FSMDefinition with a fluent API.
 *
 * Example:
 *     auto def = FSMDefinitionBuilder()
 *         .state("Idle")
 *         .state("Walk")
 *         .state("Run")
 *         .state("Attack", 0.5f)  // 0.5s duration, auto-transitions
 *         .transition("Idle", "Walk", "StartWalk")
 *         .transition("Walk", "Idle", "Stop")
 *         .transition("Walk", "Run", "Sprint")
 *         .transition("Run", "Walk", "StopSprint")
 *         .timedTransition("Attack", "Idle", 0.5f)
 *         .initialState("Idle")
 *         .build();
 */
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

    FSMDefinition build() {
        return std::move(m_definition);
    }

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
// State Name Constants - Define your states as constants
// ============================================================

/**
 * Example of how to define state constants for type safety:
 *
 *     namespace CharacterStates {
 *         constexpr uint32_t Idle = 0;
 *         constexpr uint32_t Walk = 1;
 *         constexpr uint32_t Run = 2;
 *         constexpr uint32_t Attack = 3;
 *     }
 *
 *     // In system:
 *     if (fsm.currentState == CharacterStates::Idle && fsm.justEntered) {
 *         // Play idle animation
 *     }
 */

// ============================================================
// Common FSM Events - String constants for common events
// ============================================================

namespace FSMEvents {
    constexpr const char* Start = "Start";
    constexpr const char* Stop = "Stop";
    constexpr const char* Jump = "Jump";
    constexpr const char* Land = "Land";
    constexpr const char* Attack = "Attack";
    constexpr const char* Hurt = "Hurt";
    constexpr const char* Die = "Die";
    constexpr const char* Respawn = "Respawn";
}

} // namespace Vapor
