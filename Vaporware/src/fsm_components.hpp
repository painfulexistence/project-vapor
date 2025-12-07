#pragma once

#include "action_components.hpp"
#include <entt/entt.hpp>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// ============================================================
// FSM Transition Conditions
// ============================================================

// Condition that checks if an event was fired
struct EventCondition {
    std::string eventName;
};

// Condition that checks if a component exists on the entity
template<typename T> struct HasComponentCondition {};

// Condition with custom logic
struct CustomCondition {
    std::function<bool(entt::registry&, entt::entity)> predicate;
};

// Condition that checks if onEnter actions completed
struct ActionsCompleteCondition {};

// Variant of all possible conditions
using TransitionCondition = std::variant<EventCondition, CustomCondition, ActionsCompleteCondition>;

// ============================================================
// FSM Transition
// ============================================================

struct FSMTransition {
    std::string targetState;
    TransitionCondition condition;

    // Convenience constructors
    static FSMTransition onEvent(const std::string& target, const std::string& eventName) {
        return { target, EventCondition{ eventName } };
    }

    static FSMTransition
        onCondition(const std::string& target, std::function<bool(entt::registry&, entt::entity)> pred) {
        return { target, CustomCondition{ std::move(pred) } };
    }

    static FSMTransition onActionsComplete(const std::string& target) {
        return { target, ActionsCompleteCondition{} };
    }
};

// ============================================================
// FSM State
// ============================================================

struct FSMState {
    std::string name;

    // Actions to execute when entering this state
    std::vector<ActionComponent> onEnterActions;

    // Actions to execute when exiting this state
    std::vector<ActionComponent> onExitActions;

    // Possible transitions from this state
    std::vector<FSMTransition> transitions;

    // Builder pattern for convenience
    FSMState& enter(std::vector<ActionComponent> actions) {
        onEnterActions = std::move(actions);
        return *this;
    }

    FSMState& exit(std::vector<ActionComponent> actions) {
        onExitActions = std::move(actions);
        return *this;
    }

    FSMState& addTransition(FSMTransition t) {
        transitions.push_back(std::move(t));
        return *this;
    }
};

// ============================================================
// FSM Component
// ============================================================

enum class FSMPhase : uint8_t {
    Idle,// Waiting for state entry
    EnteringState,// Running onEnter actions
    InState,// Actions complete, checking transitions
    ExitingState// Running onExit actions
};

struct FSMComponent {
    std::vector<FSMState> states;
    std::string currentState;
    std::string previousState;
    std::string pendingState;// State to transition to after exit actions
    FSMPhase phase = FSMPhase::Idle;

    // For tracking if current actions are done
    bool actionsComplete = false;

    // Get current state definition
    const FSMState* getCurrentState() const {
        for (const auto& state : states) {
            if (state.name == currentState) return &state;
        }
        return nullptr;
    }

    FSMState* getCurrentStateMutable() {
        for (auto& state : states) {
            if (state.name == currentState) return &state;
        }
        return nullptr;
    }

    const FSMState* getState(const std::string& name) const {
        for (const auto& state : states) {
            if (state.name == name) return &state;
        }
        return nullptr;
    }
};

// ============================================================
// FSM Event Component - Used to send events to FSM
// ============================================================

struct FSMEventComponent {
    std::vector<std::string> events;

    void send(const std::string& eventName) {
        events.push_back(eventName);
    }

    bool hasEvent(const std::string& eventName) const {
        for (const auto& e : events) {
            if (e == eventName) return true;
        }
        return false;
    }

    void clear() {
        events.clear();
    }
};

// ============================================================
// Tag to mark that FSM actions are currently running
// ============================================================

struct FSMActionsRunningTag {};

// ============================================================
// FSM Builder - Fluent API for creating state machines
// ============================================================

class FSMBuilder {
public:
    FSMBuilder& state(const std::string& name) {
        m_states.push_back({ .name = name });
        m_currentStateIndex = m_states.size() - 1;
        return *this;
    }

    FSMBuilder& enter(std::vector<ActionComponent> actions) {
        if (m_currentStateIndex < m_states.size()) {
            m_states[m_currentStateIndex].onEnterActions = std::move(actions);
        }
        return *this;
    }

    FSMBuilder& exit(std::vector<ActionComponent> actions) {
        if (m_currentStateIndex < m_states.size()) {
            m_states[m_currentStateIndex].onExitActions = std::move(actions);
        }
        return *this;
    }

    FSMBuilder& transitionTo(const std::string& target, const std::string& onEvent) {
        if (m_currentStateIndex < m_states.size()) {
            m_states[m_currentStateIndex].transitions.push_back(FSMTransition::onEvent(target, onEvent));
        }
        return *this;
    }

    FSMBuilder& transitionTo(const std::string& target, std::function<bool(entt::registry&, entt::entity)> condition) {
        if (m_currentStateIndex < m_states.size()) {
            m_states[m_currentStateIndex].transitions.push_back(FSMTransition::onCondition(target, std::move(condition))
            );
        }
        return *this;
    }

    FSMBuilder& transitionOnComplete(const std::string& target) {
        if (m_currentStateIndex < m_states.size()) {
            m_states[m_currentStateIndex].transitions.push_back(FSMTransition::onActionsComplete(target));
        }
        return *this;
    }

    FSMBuilder& initialState(const std::string& name) {
        m_initialState = name;
        return *this;
    }

    FSMComponent build() {
        FSMComponent fsm;
        fsm.states = std::move(m_states);
        fsm.currentState = m_initialState;
        fsm.phase = FSMPhase::Idle;
        return fsm;
    }

private:
    std::vector<FSMState> m_states;
    size_t m_currentStateIndex = 0;
    std::string m_initialState;
};
