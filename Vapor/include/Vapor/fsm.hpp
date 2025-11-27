#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <any>
#include <optional>

namespace Vapor {

// Forward declarations
class FSM;
class State;
class StateAction;

/**
 * @brief Base class for composable state actions.
 *
 * Extend this class to create reusable actions that can be added to states.
 * Actions execute during state enter, update, or exit phases.
 *
 * Example:
 * @code
 * class SetVelocityAction : public StateAction {
 * public:
 *     SetVelocityAction(float x, float y) : velocity{x, y} {}
 *
 *     void execute(FSM* fsm, float dt = 0.0f) override {
 *         // Access owner and modify velocity
 *         auto* owner = fsm->getOwner<GameObject>();
 *         if (owner) {
 *             owner->velocity = velocity;
 *         }
 *     }
 *
 * private:
 *     glm::vec2 velocity;
 * };
 * @endcode
 */
class StateAction {
public:
    virtual ~StateAction() = default;

    /**
     * @brief Execute the action (override in subclasses).
     *
     * @param fsm FSM instance containing the current state
     * @param dt Delta time since last frame (0 for enter/exit actions)
     */
    virtual void execute(FSM* fsm, float dt = 0.0f) = 0;
};

/**
 * @brief State class for finite state machines.
 *
 * States contain lists of actions that execute on enter, update, and exit.
 * This composable approach allows building complex behaviors by combining
 * simple, reusable actions.
 *
 * Example:
 * @code
 * auto idleState = std::make_shared<State>("Idle");
 * idleState->addEnterAction(std::make_unique<SetAnimationAction>("idle"));
 * idleState->addUpdateAction(std::make_unique<ApplyGravityAction>());
 * idleState->addExitAction(std::make_unique<StopSoundAction>());
 *
 * fsm->addState(idleState);
 * @endcode
 */
class State {
public:
    /**
     * @brief Initialize state with a name.
     *
     * @param name Unique identifier for this state
     */
    explicit State(const std::string& name);
    virtual ~State() = default;

    /**
     * @brief Add action to execute when entering this state.
     *
     * @param action StateAction instance to execute on enter
     */
    void addEnterAction(std::unique_ptr<StateAction> action);

    /**
     * @brief Add action to execute every frame while in this state.
     *
     * @param action StateAction instance to execute on update
     */
    void addUpdateAction(std::unique_ptr<StateAction> action);

    /**
     * @brief Add action to execute when exiting this state.
     *
     * @param action StateAction instance to execute on exit
     */
    void addExitAction(std::unique_ptr<StateAction> action);

    /**
     * @brief Execute all enter actions (called by FSM when entering this state).
     *
     * @param fsm FSM instance containing this state
     */
    virtual void enter(FSM* fsm);

    /**
     * @brief Execute all update actions (called by FSM every frame).
     *
     * @param fsm FSM instance containing this state
     * @param dt Delta time since last frame
     */
    virtual void update(FSM* fsm, float dt);

    /**
     * @brief Execute all exit actions (called by FSM when leaving this state).
     *
     * @param fsm FSM instance containing this state
     */
    virtual void exit(FSM* fsm);

    /**
     * @brief Check if transition to target state is allowed (override for custom logic).
     *
     * @param targetState Name of state to transition to
     * @return true if transition is allowed, false otherwise
     */
    virtual bool canTransitionTo(const std::string& targetState) const;

    /**
     * @brief Get the name of this state.
     *
     * @return State name
     */
    const std::string& getName() const { return name; }

protected:
    std::string name;
    std::vector<std::unique_ptr<StateAction>> enterActions;
    std::vector<std::unique_ptr<StateAction>> updateActions;
    std::vector<std::unique_ptr<StateAction>> exitActions;
    std::unordered_set<std::string> activeActionTags;  // Track action tags started by this state
};

/**
 * @brief Event type for FSM transitions.
 *
 * Simple event structure that can be extended for game-specific needs.
 */
struct FSMEvent {
    std::string type;
    std::any data;  // Optional event data

    FSMEvent(const std::string& type) : type(type) {}
    FSMEvent(const std::string& type, std::any data) : type(type), data(std::move(data)) {}
};

/**
 * @brief Finite State Machine for managing object behavior.
 *
 * Supports:
 * - State management with enter/update/exit lifecycle
 * - Event-driven transitions via transition table
 * - State history tracking
 * - Extensible for game-specific needs
 *
 * Example:
 * @code
 * auto fsm = std::make_unique<FSM>();
 * fsm->setOwner(&player);
 *
 * auto idle = std::make_shared<State>("Idle");
 * auto jump = std::make_shared<State>("Jump");
 * fsm->addState(idle);
 * fsm->addState(jump);
 *
 * fsm->addTransition("Idle", "JumpPressed", "Jump");
 * fsm->setState("Idle");
 *
 * // Later, when event occurs:
 * fsm->handleEvent(FSMEvent("JumpPressed"));
 * @endcode
 */
class FSM {
public:
    FSM();
    virtual ~FSM() = default;

    /**
     * @brief Set the owner object for this FSM.
     *
     * @param owner Pointer to the owning object (GameObject, Entity, etc.)
     */
    void setOwner(void* owner) { this->owner = owner; }

    /**
     * @brief Get the owner object (type-safe).
     *
     * @tparam T Type of the owner object
     * @return Pointer to owner cast to type T, or nullptr if owner is null
     */
    template<typename T>
    T* getOwner() const { return static_cast<T*>(owner); }

    /**
     * @brief Add a state to the FSM.
     *
     * @param state State instance to add
     */
    void addState(std::shared_ptr<State> state);

    /**
     * @brief Define a state transition triggered by an event.
     *
     * @param fromStateName Source state name
     * @param eventType Event type that triggers transition
     * @param toStateName Target state name
     */
    void addTransition(const std::string& fromStateName, const std::string& eventType, const std::string& toStateName);

    /**
     * @brief Change to a different state.
     *
     * Handles exit of current state and enter of new state. Can re-enter
     * the same state to reset it (e.g., for hit states with timers).
     *
     * @param newStateName Name of state to transition to
     */
    void changeState(const std::string& newStateName);

    /**
     * @brief Set initial state without triggering exit (alias for changeState).
     *
     * @param stateName Name of state to set as current
     */
    void setState(const std::string& stateName);

    /**
     * @brief Set a variable in the FSM.
     *
     * @param variableName Name of variable to set
     * @param value Value to set
     */
    void setVariable(const std::string& variableName, std::any value);

    /**
     * @brief Get a variable from the FSM.
     *
     * @tparam T Type to cast the variable to
     * @param variableName Name of variable to get
     * @return Optional containing the value if found and type matches
     */
    template<typename T>
    std::optional<T> getVariable(const std::string& variableName) const {
        auto it = variables.find(variableName);
        if (it != variables.end()) {
            try {
                return std::any_cast<T>(it->second);
            } catch (const std::bad_any_cast&) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief Handle an event and trigger state transitions if defined.
     *
     * Checks transition table for current state. If event matches a defined
     * transition, changes to target state.
     *
     * @param event Event object with type
     */
    virtual void handleEvent(const FSMEvent& event);

    /**
     * @brief Update current state (call every frame).
     *
     * @param dt Delta time since last frame
     */
    void update(float dt);

    /**
     * @brief Get name of current state.
     *
     * @return Current state name or empty string if no state set
     */
    std::string getCurrentStateName() const;

    /**
     * @brief Get current state.
     *
     * @return Shared pointer to current state, or nullptr if no state set
     */
    std::shared_ptr<State> getCurrentState() const { return currentState; }

    /**
     * @brief Print all defined state transitions (useful for debugging).
     */
    void printStateDiagram() const;

protected:
    void* owner = nullptr;  // The GameObject/Entity this FSM belongs to
    std::shared_ptr<State> currentState;
    std::unordered_map<std::string, std::shared_ptr<State>> states;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> transitions;
    std::unordered_map<std::string, std::any> variables;
    std::vector<std::string> stateHistory;
};

// ============================================================================
// Example StateAction implementations
// ============================================================================

/**
 * @brief Example action that executes a callback function.
 *
 * Useful for simple one-off actions without creating a new class.
 */
class CallbackAction : public StateAction {
public:
    using Callback = std::function<void(FSM*, float)>;

    explicit CallbackAction(Callback callback) : callback(std::move(callback)) {}

    void execute(FSM* fsm, float dt = 0.0f) override {
        if (callback) {
            callback(fsm, dt);
        }
    }

private:
    Callback callback;
};

/**
 * @brief Example action that prints a message.
 *
 * Useful for debugging state transitions.
 */
class PrintAction : public StateAction {
public:
    explicit PrintAction(const std::string& message) : message(message) {}

    void execute(FSM* fsm, float dt = 0.0f) override;

private:
    std::string message;
};

} // namespace Vapor
