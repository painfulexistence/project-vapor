#include "Vapor/fsm.hpp"
#include <fmt/core.h>
#include <iostream>

namespace Vapor {

// ============================================================================
// State Implementation
// ============================================================================

State::State(const std::string& name) : name(name) {}

void State::addEnterAction(std::unique_ptr<StateAction> action) {
    enterActions.push_back(std::move(action));
}

void State::addUpdateAction(std::unique_ptr<StateAction> action) {
    updateActions.push_back(std::move(action));
}

void State::addExitAction(std::unique_ptr<StateAction> action) {
    exitActions.push_back(std::move(action));
}

void State::enter(FSM* fsm) {
    for (auto& action : enterActions) {
        action->execute(fsm);
    }
}

void State::update(FSM* fsm, float dt) {
    for (auto& action : updateActions) {
        action->execute(fsm, dt);
    }
}

void State::exit(FSM* fsm) {
    // Clean up active action tags (if action manager exists in the future)
    activeActionTags.clear();

    for (auto& action : exitActions) {
        action->execute(fsm);
    }
}

bool State::canTransitionTo(const std::string& targetState) const {
    return true;  // Default: allow all transitions
}

// ============================================================================
// FSM Implementation
// ============================================================================

FSM::FSM() : owner(nullptr), currentState(nullptr) {}

void FSM::addState(std::shared_ptr<State> state) {
    states[state->getName()] = std::move(state);
}

void FSM::addTransition(const std::string& fromStateName, const std::string& eventType, const std::string& toStateName) {
    transitions[fromStateName][eventType] = toStateName;
}

void FSM::changeState(const std::string& newStateName) {
    auto it = states.find(newStateName);
    if (it == states.end()) {
        fmt::print("Error: state '{}' not found\n", newStateName);
        return;
    }

    auto newState = it->second;

    // Check if transition is allowed
    if (currentState && !currentState->canTransitionTo(newStateName)) {
        fmt::print("Error: cannot transition from '{}' to '{}'\n",
                   currentState->getName(), newStateName);
        return;
    }

    // Exit current state (unless transitioning to same state)
    if (currentState && currentState->getName() != newStateName) {
        currentState->exit(this);
    }

    // Enter new state (always, even if same state to allow reset)
    currentState = newState;
    currentState->enter(this);
}

void FSM::setState(const std::string& stateName) {
    changeState(stateName);
}

void FSM::setVariable(const std::string& variableName, std::any value) {
    variables[variableName] = std::move(value);
}

void FSM::handleEvent(const FSMEvent& event) {
    if (!currentState) {
        return;
    }

    const std::string& currentStateName = currentState->getName();

    // Check transition table
    auto stateIt = transitions.find(currentStateName);
    if (stateIt != transitions.end()) {
        auto eventIt = stateIt->second.find(event.type);
        if (eventIt != stateIt->second.end()) {
            const std::string& targetState = eventIt->second;
            changeState(targetState);
            return;
        }
    }
}

void FSM::update(float dt) {
    if (currentState) {
        currentState->update(this, dt);
    }
}

std::string FSM::getCurrentStateName() const {
    return currentState ? currentState->getName() : "";
}

void FSM::printStateDiagram() const {
    fmt::print("\n=== State Diagram ===\n");
    for (const auto& [fromState, eventMap] : transitions) {
        for (const auto& [eventType, toState] : eventMap) {
            fmt::print("{} --[{}]--> {}\n", fromState, eventType, toState);
        }
    }
}

// ============================================================================
// Example StateAction Implementations
// ============================================================================

void PrintAction::execute(FSM* fsm, float dt) {
    fmt::print("{}\n", message);
}

} // namespace Vapor
