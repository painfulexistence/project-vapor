#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include <string>
#include <queue>
#include <any>
#include <optional>
#include <typeindex>

namespace Vapor {

// Forward declarations
class FSMState;
class FSMStateMachine;
class ActionManager;

// ============================================================
// FSM Event System
// ============================================================

/**
 * Event identifier using string names for flexibility.
 * Events can carry optional payload data.
 */
struct FSMEvent {
    std::string name;
    std::any payload;

    FSMEvent() = default;
    explicit FSMEvent(std::string eventName) : name(std::move(eventName)) {}

    template<typename T>
    FSMEvent(std::string eventName, T data)
        : name(std::move(eventName)), payload(std::move(data)) {}

    template<typename T>
    std::optional<T> getPayload() const {
        if (payload.has_value()) {
            try {
                return std::any_cast<T>(payload);
            } catch (const std::bad_any_cast&) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    bool operator==(const FSMEvent& other) const {
        return name == other.name;
    }
};

// ============================================================
// FSM Transition
// ============================================================

/**
 * Represents a transition between states.
 *
 * Transitions can be triggered by:
 * - Events (event-based transition)
 * - Conditions (condition-based transition, evaluated each frame)
 * - Both (event triggers, but condition must also be met)
 */
struct FSMTransition {
    using ConditionFunc = std::function<bool()>;

    std::string targetState;
    std::string triggerEvent;           // Event that triggers this transition (empty = condition-only)
    ConditionFunc condition = nullptr;  // Optional condition function

    FSMTransition() = default;

    // Event-triggered transition
    FSMTransition(std::string target, std::string event)
        : targetState(std::move(target)), triggerEvent(std::move(event)) {}

    // Condition-triggered transition
    FSMTransition(std::string target, ConditionFunc cond)
        : targetState(std::move(target)), condition(std::move(cond)) {}

    // Event + condition transition
    FSMTransition(std::string target, std::string event, ConditionFunc cond)
        : targetState(std::move(target)), triggerEvent(std::move(event)), condition(std::move(cond)) {}

    bool canTransition(const FSMEvent* event = nullptr) const {
        // Check event match if this is an event-triggered transition
        if (!triggerEvent.empty()) {
            if (!event || event->name != triggerEvent) {
                return false;
            }
        }

        // Check condition if present
        if (condition && !condition()) {
            return false;
        }

        return true;
    }
};

// ============================================================
// FSM State Base Class
// ============================================================

/**
 * Base class for FSM states.
 *
 * Override the virtual methods to define state behavior:
 * - onEnter(): Called when entering the state
 * - onUpdate(): Called every frame while in this state
 * - onExit(): Called when leaving the state
 *
 * Example:
 *     class IdleState : public FSMState {
 *     public:
 *         IdleState() : FSMState("Idle") {}
 *
 *         void onEnter() override {
 *             // Start idle animation
 *         }
 *
 *         void onUpdate(float dt) override {
 *             // Check for input, etc.
 *         }
 *
 *         void onExit() override {
 *             // Clean up
 *         }
 *     };
 */
class FSMState {
public:
    explicit FSMState(std::string name) : m_name(std::move(name)) {}
    virtual ~FSMState() = default;

    // Lifecycle callbacks
    virtual void onEnter() {}
    virtual void onUpdate(float dt) {}
    virtual void onExit() {}

    // Event handling (return true if event was consumed)
    virtual bool onEvent(const FSMEvent& event) { return false; }

    // State identification
    const std::string& getName() const { return m_name; }

    // Add a transition from this state
    void addTransition(FSMTransition transition) {
        m_transitions.push_back(std::move(transition));
    }

    // Add event-triggered transition
    void addTransition(const std::string& targetState, const std::string& triggerEvent) {
        m_transitions.emplace_back(targetState, triggerEvent);
    }

    // Add condition-triggered transition
    void addTransition(const std::string& targetState, FSMTransition::ConditionFunc condition) {
        m_transitions.emplace_back(targetState, std::move(condition));
    }

    // Get all transitions
    const std::vector<FSMTransition>& getTransitions() const { return m_transitions; }

    // Access to parent state machine (set by FSMStateMachine)
    FSMStateMachine* getStateMachine() const { return m_stateMachine; }

    // Helper to send event to own state machine
    void sendEvent(const FSMEvent& event);
    void sendEvent(const std::string& eventName);

protected:
    std::string m_name;
    std::vector<FSMTransition> m_transitions;
    FSMStateMachine* m_stateMachine = nullptr;

    friend class FSMStateMachine;
};

// ============================================================
// FSM State Machine
// ============================================================

/**
 * A finite state machine that manages states and transitions.
 *
 * Example:
 *     auto fsm = std::make_shared<FSMStateMachine>();
 *
 *     // Add states
 *     auto idle = std::make_shared<IdleState>();
 *     auto walk = std::make_shared<WalkState>();
 *     auto run = std::make_shared<RunState>();
 *
 *     // Define transitions
 *     idle->addTransition("Walk", "StartWalking");
 *     walk->addTransition("Idle", "Stop");
 *     walk->addTransition("Run", "StartRunning");
 *     run->addTransition("Walk", "StopRunning");
 *
 *     fsm->addState(idle);
 *     fsm->addState(walk);
 *     fsm->addState(run);
 *     fsm->setInitialState("Idle");
 *
 *     // In update loop:
 *     fsm->update(deltaTime);
 *
 *     // Trigger transitions:
 *     fsm->sendEvent("StartWalking");
 */
class FSMStateMachine {
public:
    FSMStateMachine() = default;
    ~FSMStateMachine() = default;

    // State management
    void addState(std::shared_ptr<FSMState> state);
    void setInitialState(const std::string& stateName);

    // Event handling
    void sendEvent(const FSMEvent& event);
    void sendEvent(const std::string& eventName);

    template<typename T>
    void sendEvent(const std::string& eventName, T payload) {
        sendEvent(FSMEvent(eventName, std::move(payload)));
    }

    // Force transition to a specific state (ignores conditions)
    void forceTransition(const std::string& stateName);

    // Update the state machine
    void update(float dt);

    // Query current state
    const std::string& getCurrentStateName() const;
    FSMState* getCurrentState() const { return m_currentState; }
    bool isInState(const std::string& stateName) const;

    // Check if initialized and running
    bool isRunning() const { return m_currentState != nullptr; }

    // Get state by name
    FSMState* getState(const std::string& stateName) const;

    // Variables (shared state data accessible from all states)
    template<typename T>
    void setVariable(const std::string& name, T value) {
        m_variables[name] = std::move(value);
    }

    template<typename T>
    std::optional<T> getVariable(const std::string& name) const {
        auto it = m_variables.find(name);
        if (it != m_variables.end()) {
            try {
                return std::any_cast<T>(it->second);
            } catch (const std::bad_any_cast&) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    template<typename T>
    T getVariableOr(const std::string& name, T defaultValue) const {
        auto result = getVariable<T>(name);
        return result.value_or(defaultValue);
    }

    bool hasVariable(const std::string& name) const {
        return m_variables.find(name) != m_variables.end();
    }

    void clearVariable(const std::string& name) {
        m_variables.erase(name);
    }

    // State history
    const std::vector<std::string>& getStateHistory() const { return m_stateHistory; }

    // Enable/disable history tracking
    void setHistoryEnabled(bool enabled) { m_historyEnabled = enabled; }
    bool isHistoryEnabled() const { return m_historyEnabled; }

    // Go back to previous state (if history enabled)
    bool goBack();

private:
    void processEvents();
    void checkConditionTransitions();
    void transitionTo(const std::string& stateName);

    std::unordered_map<std::string, std::shared_ptr<FSMState>> m_states;
    FSMState* m_currentState = nullptr;
    std::string m_initialStateName;

    std::queue<FSMEvent> m_eventQueue;
    std::unordered_map<std::string, std::any> m_variables;

    std::vector<std::string> m_stateHistory;
    bool m_historyEnabled = false;
    static constexpr size_t MAX_HISTORY_SIZE = 32;
};

// ============================================================
// Built-in State Types
// ============================================================

/**
 * A state that automatically transitions after a duration.
 */
class TimedState : public FSMState {
public:
    TimedState(std::string name, float duration, std::string nextState)
        : FSMState(std::move(name))
        , m_duration(duration)
        , m_nextState(std::move(nextState)) {}

    void onEnter() override {
        m_elapsed = 0.0f;
    }

    void onUpdate(float dt) override {
        m_elapsed += dt;
        if (m_elapsed >= m_duration) {
            sendEvent("_TimedComplete");
        }
    }

    float getProgress() const {
        return m_duration > 0.0f ? std::min(m_elapsed / m_duration, 1.0f) : 1.0f;
    }

    float getElapsed() const { return m_elapsed; }
    float getDuration() const { return m_duration; }

protected:
    float m_duration;
    float m_elapsed = 0.0f;
    std::string m_nextState;

    void setupAutoTransition() {
        addTransition(m_nextState, "_TimedComplete");
    }
};

/**
 * A state that waits for a specific event before transitioning.
 */
class WaitForEventState : public FSMState {
public:
    WaitForEventState(std::string name, std::string waitEvent, std::string nextState)
        : FSMState(std::move(name))
        , m_waitEvent(std::move(waitEvent))
        , m_nextState(std::move(nextState)) {
        addTransition(m_nextState, m_waitEvent);
    }

protected:
    std::string m_waitEvent;
    std::string m_nextState;
};

/**
 * A state that executes a callback on enter and immediately transitions.
 */
class CallbackState : public FSMState {
public:
    using Callback = std::function<void()>;

    CallbackState(std::string name, Callback callback, std::string nextState)
        : FSMState(std::move(name))
        , m_callback(std::move(callback))
        , m_nextState(std::move(nextState)) {
        addTransition(m_nextState, "_CallbackComplete");
    }

    void onEnter() override {
        if (m_callback) {
            m_callback();
        }
        sendEvent("_CallbackComplete");
    }

protected:
    Callback m_callback;
    std::string m_nextState;
};

/**
 * A choice/branch state that evaluates conditions and transitions accordingly.
 */
class ChoiceState : public FSMState {
public:
    using ChoiceFunc = std::function<std::string()>;

    ChoiceState(std::string name, ChoiceFunc choiceFunc)
        : FSMState(std::move(name))
        , m_choiceFunc(std::move(choiceFunc)) {}

    void onEnter() override {
        if (m_choiceFunc) {
            std::string nextState = m_choiceFunc();
            if (!nextState.empty()) {
                getStateMachine()->forceTransition(nextState);
            }
        }
    }

protected:
    ChoiceFunc m_choiceFunc;
};

// ============================================================
// FSM Component (for ECS integration)
// ============================================================

/**
 * ECS component that holds a state machine.
 *
 * Example:
 *     auto entity = registry.create();
 *     auto& fsmComp = registry.emplace<FSMComponent>(entity);
 *
 *     auto idle = std::make_shared<IdleState>();
 *     auto walk = std::make_shared<WalkState>();
 *     idle->addTransition("Walk", "StartWalk");
 *     walk->addTransition("Idle", "Stop");
 *
 *     fsmComp.fsm->addState(idle);
 *     fsmComp.fsm->addState(walk);
 *     fsmComp.fsm->setInitialState("Idle");
 */
struct FSMComponent {
    std::shared_ptr<FSMStateMachine> fsm = std::make_shared<FSMStateMachine>();

    // Convenience methods
    void sendEvent(const FSMEvent& event) {
        if (fsm) fsm->sendEvent(event);
    }

    void sendEvent(const std::string& eventName) {
        if (fsm) fsm->sendEvent(eventName);
    }

    template<typename T>
    void sendEvent(const std::string& eventName, T payload) {
        if (fsm) fsm->sendEvent(eventName, std::move(payload));
    }

    const std::string& getCurrentStateName() const {
        static const std::string empty;
        return fsm ? fsm->getCurrentStateName() : empty;
    }

    bool isInState(const std::string& stateName) const {
        return fsm && fsm->isInState(stateName);
    }
};

// ============================================================
// FSM Builder (Fluent API for easy FSM construction)
// ============================================================

/**
 * Builder pattern for constructing FSMs with a fluent API.
 *
 * Example:
 *     auto fsm = FSMBuilder()
 *         .state<IdleState>("Idle")
 *             .on("Walk").transitionTo("Walking")
 *             .on("Run").transitionTo("Running")
 *         .state<WalkState>("Walking")
 *             .on("Stop").transitionTo("Idle")
 *             .on("Run").transitionTo("Running")
 *         .state<RunState>("Running")
 *             .on("Stop").transitionTo("Idle")
 *             .when([]{ return stamina <= 0; }).transitionTo("Walking")
 *         .initialState("Idle")
 *         .build();
 */
class FSMBuilder {
public:
    class StateBuilder;
    class TransitionBuilder;

    FSMBuilder() : m_fsm(std::make_shared<FSMStateMachine>()) {}

    // Add a custom state
    template<typename StateType, typename... Args>
    StateBuilder state(Args&&... args) {
        auto state = std::make_shared<StateType>(std::forward<Args>(args)...);
        m_currentState = state;
        m_fsm->addState(state);
        return StateBuilder(*this, state);
    }

    // Add a simple named state
    StateBuilder state(const std::string& name) {
        auto state = std::make_shared<FSMState>(name);
        m_currentState = state;
        m_fsm->addState(state);
        return StateBuilder(*this, state);
    }

    // Set initial state
    FSMBuilder& initialState(const std::string& stateName) {
        m_fsm->setInitialState(stateName);
        return *this;
    }

    // Enable state history
    FSMBuilder& enableHistory(bool enabled = true) {
        m_fsm->setHistoryEnabled(enabled);
        return *this;
    }

    // Build and return the FSM
    std::shared_ptr<FSMStateMachine> build() {
        return m_fsm;
    }

    // Nested builder classes
    class TransitionBuilder {
    public:
        TransitionBuilder(StateBuilder& parent, std::string event)
            : m_parent(parent), m_event(std::move(event)) {}

        StateBuilder& transitionTo(const std::string& targetState) {
            m_parent.m_state->addTransition(targetState, m_event);
            return m_parent;
        }

    private:
        StateBuilder& m_parent;
        std::string m_event;
    };

    class ConditionTransitionBuilder {
    public:
        ConditionTransitionBuilder(StateBuilder& parent, FSMTransition::ConditionFunc condition)
            : m_parent(parent), m_condition(std::move(condition)) {}

        StateBuilder& transitionTo(const std::string& targetState) {
            m_parent.m_state->addTransition(targetState, m_condition);
            return m_parent;
        }

    private:
        StateBuilder& m_parent;
        FSMTransition::ConditionFunc m_condition;
    };

    class StateBuilder {
    public:
        StateBuilder(FSMBuilder& parent, std::shared_ptr<FSMState> state)
            : m_parent(parent), m_state(std::move(state)) {}

        // Add event-triggered transition
        TransitionBuilder on(const std::string& eventName) {
            return TransitionBuilder(*this, eventName);
        }

        // Add condition-triggered transition
        ConditionTransitionBuilder when(FSMTransition::ConditionFunc condition) {
            return ConditionTransitionBuilder(*this, std::move(condition));
        }

        // Return to parent builder for next state
        template<typename StateType, typename... Args>
        StateBuilder state(Args&&... args) {
            return m_parent.state<StateType>(std::forward<Args>(args)...);
        }

        StateBuilder state(const std::string& name) {
            return m_parent.state(name);
        }

        FSMBuilder& initialState(const std::string& stateName) {
            return m_parent.initialState(stateName);
        }

        FSMBuilder& enableHistory(bool enabled = true) {
            return m_parent.enableHistory(enabled);
        }

        std::shared_ptr<FSMStateMachine> build() {
            return m_parent.build();
        }

    private:
        FSMBuilder& m_parent;
        std::shared_ptr<FSMState> m_state;

        friend class TransitionBuilder;
        friend class ConditionTransitionBuilder;
    };

private:
    std::shared_ptr<FSMStateMachine> m_fsm;
    std::shared_ptr<FSMState> m_currentState;
};

// ============================================================
// Inline Implementations
// ============================================================

inline void FSMState::sendEvent(const FSMEvent& event) {
    if (m_stateMachine) {
        m_stateMachine->sendEvent(event);
    }
}

inline void FSMState::sendEvent(const std::string& eventName) {
    if (m_stateMachine) {
        m_stateMachine->sendEvent(eventName);
    }
}

inline void FSMStateMachine::addState(std::shared_ptr<FSMState> state) {
    if (state) {
        state->m_stateMachine = this;
        m_states[state->getName()] = std::move(state);
    }
}

inline void FSMStateMachine::setInitialState(const std::string& stateName) {
    m_initialStateName = stateName;
    if (!m_currentState && m_states.find(stateName) != m_states.end()) {
        transitionTo(stateName);
    }
}

inline void FSMStateMachine::sendEvent(const FSMEvent& event) {
    m_eventQueue.push(event);
}

inline void FSMStateMachine::sendEvent(const std::string& eventName) {
    m_eventQueue.push(FSMEvent(eventName));
}

inline void FSMStateMachine::forceTransition(const std::string& stateName) {
    transitionTo(stateName);
}

inline void FSMStateMachine::update(float dt) {
    if (!m_currentState) {
        if (!m_initialStateName.empty()) {
            transitionTo(m_initialStateName);
        }
        return;
    }

    // Process queued events first
    processEvents();

    // Check condition-based transitions
    checkConditionTransitions();

    // Update current state
    if (m_currentState) {
        m_currentState->onUpdate(dt);
    }
}

inline const std::string& FSMStateMachine::getCurrentStateName() const {
    static const std::string empty;
    return m_currentState ? m_currentState->getName() : empty;
}

inline bool FSMStateMachine::isInState(const std::string& stateName) const {
    return m_currentState && m_currentState->getName() == stateName;
}

inline FSMState* FSMStateMachine::getState(const std::string& stateName) const {
    auto it = m_states.find(stateName);
    return it != m_states.end() ? it->second.get() : nullptr;
}

inline bool FSMStateMachine::goBack() {
    if (!m_historyEnabled || m_stateHistory.empty()) {
        return false;
    }

    std::string previousState = m_stateHistory.back();
    m_stateHistory.pop_back();

    // Temporarily disable history to avoid recording this transition
    bool wasEnabled = m_historyEnabled;
    m_historyEnabled = false;
    transitionTo(previousState);
    m_historyEnabled = wasEnabled;

    return true;
}

inline void FSMStateMachine::processEvents() {
    while (!m_eventQueue.empty()) {
        FSMEvent event = std::move(m_eventQueue.front());
        m_eventQueue.pop();

        if (!m_currentState) continue;

        // Let state handle event first
        bool consumed = m_currentState->onEvent(event);

        // Check for event-triggered transitions
        if (!consumed) {
            for (const auto& transition : m_currentState->getTransitions()) {
                if (transition.canTransition(&event)) {
                    transitionTo(transition.targetState);
                    break;
                }
            }
        }
    }
}

inline void FSMStateMachine::checkConditionTransitions() {
    if (!m_currentState) return;

    for (const auto& transition : m_currentState->getTransitions()) {
        // Only check transitions that are condition-only (no event trigger)
        if (transition.triggerEvent.empty() && transition.canTransition()) {
            transitionTo(transition.targetState);
            break;
        }
    }
}

inline void FSMStateMachine::transitionTo(const std::string& stateName) {
    auto it = m_states.find(stateName);
    if (it == m_states.end()) {
        return;
    }

    FSMState* newState = it->second.get();

    if (m_currentState) {
        // Record history before transition
        if (m_historyEnabled) {
            m_stateHistory.push_back(m_currentState->getName());
            if (m_stateHistory.size() > MAX_HISTORY_SIZE) {
                m_stateHistory.erase(m_stateHistory.begin());
            }
        }

        m_currentState->onExit();
    }

    m_currentState = newState;

    if (m_currentState) {
        m_currentState->onEnter();
    }
}

} // namespace Vapor
