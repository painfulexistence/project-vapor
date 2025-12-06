#pragma once

#include "animation_components.hpp"
#include "fsm_components.hpp"
#include <entt/entt.hpp>
#include <fmt/core.h>

// ============================================================
// FSM System - Manages state transitions via Component emplacement
//
// This system follows ECS principles:
// - Only manages state transition logic
// - Triggers actions by emplacing ActionQueueComponent
// - Lets AnimationSystem handle the actual execution
// ============================================================

class FSMSystem {
public:
    static void update(entt::registry& reg) {
        auto view = reg.view<FSMComponent>();

        for (auto entity : view) {
            auto& fsm = view.get<FSMComponent>(entity);
            updateEntity(reg, entity, fsm);
        }

        // Clear events at end of frame
        clearEvents(reg);
    }

    // Send an event to a specific entity's FSM
    static void sendEvent(entt::registry& reg, entt::entity entity, const std::string& eventName) {
        auto& events = reg.get_or_emplace<FSMEventComponent>(entity);
        events.send(eventName);
    }

    // Send an event to all FSMs
    static void broadcastEvent(entt::registry& reg, const std::string& eventName) {
        auto view = reg.view<FSMComponent>();
        for (auto entity : view) {
            sendEvent(reg, entity, eventName);
        }
    }

    // Force transition to a state (bypasses conditions)
    static void forceTransition(entt::registry& reg, entt::entity entity, const std::string& stateName) {
        if (auto* fsm = reg.try_get<FSMComponent>(entity)) {
            beginTransition(reg, entity, *fsm, stateName);
        }
    }

private:
    static void updateEntity(entt::registry& reg, entt::entity entity, FSMComponent& fsm) {
        switch (fsm.phase) {
            case FSMPhase::Idle:
                // Start entering the initial state
                if (!fsm.currentState.empty()) {
                    beginEnterState(reg, entity, fsm);
                }
                break;

            case FSMPhase::EnteringState:
                // Check if enter actions completed
                if (checkActionsComplete(reg, entity)) {
                    fsm.phase = FSMPhase::InState;
                    reg.remove<FSMActionsRunningTag>(entity);
                }
                break;

            case FSMPhase::InState:
                // Check transitions
                checkTransitions(reg, entity, fsm);
                break;

            case FSMPhase::ExitingState:
                // Check if exit actions completed
                if (checkActionsComplete(reg, entity)) {
                    reg.remove<FSMActionsRunningTag>(entity);
                    // Now enter the pending state
                    fsm.currentState = fsm.pendingState;
                    fsm.pendingState.clear();
                    beginEnterState(reg, entity, fsm);
                }
                break;
        }
    }

    static void beginEnterState(entt::registry& reg, entt::entity entity, FSMComponent& fsm) {
        const FSMState* state = fsm.getCurrentState();
        if (!state) {
            fmt::print(stderr, "FSM: State '{}' not found\n", fsm.currentState);
            fsm.phase = FSMPhase::InState;
            return;
        }

        if (state->onEnterActions.empty()) {
            // No enter actions, go directly to InState
            fsm.phase = FSMPhase::InState;
        } else {
            // Emplace ActionQueueComponent for AnimationSystem to execute
            emplaceActions(reg, entity, state->onEnterActions, "fsm_enter");
            reg.emplace_or_replace<FSMActionsRunningTag>(entity);
            fsm.phase = FSMPhase::EnteringState;
        }
    }

    static void beginExitState(entt::registry& reg, entt::entity entity, FSMComponent& fsm,
                                const std::string& nextState) {
        const FSMState* state = fsm.getCurrentState();

        fsm.previousState = fsm.currentState;
        fsm.pendingState = nextState;

        if (!state || state->onExitActions.empty()) {
            // No exit actions, go directly to entering new state
            fsm.currentState = nextState;
            fsm.pendingState.clear();
            beginEnterState(reg, entity, fsm);
        } else {
            // Emplace exit actions
            emplaceActions(reg, entity, state->onExitActions, "fsm_exit");
            reg.emplace_or_replace<FSMActionsRunningTag>(entity);
            fsm.phase = FSMPhase::ExitingState;
        }
    }

    static void beginTransition(entt::registry& reg, entt::entity entity, FSMComponent& fsm,
                                 const std::string& targetState) {
        if (fsm.currentState == targetState) return;  // Already in target state

        beginExitState(reg, entity, fsm, targetState);
    }

    static void checkTransitions(entt::registry& reg, entt::entity entity, FSMComponent& fsm) {
        const FSMState* state = fsm.getCurrentState();
        if (!state) return;

        const FSMEventComponent* events = reg.try_get<FSMEventComponent>(entity);

        for (const auto& transition : state->transitions) {
            bool shouldTransition = std::visit([&](const auto& cond) -> bool {
                return checkCondition(reg, entity, fsm, events, cond);
            }, transition.condition);

            if (shouldTransition) {
                beginTransition(reg, entity, fsm, transition.targetState);
                break;  // Only one transition per frame
            }
        }
    }

    // Condition checkers for each type
    static bool checkCondition(entt::registry& reg, entt::entity entity, FSMComponent& fsm,
                                const FSMEventComponent* events, const EventCondition& cond) {
        return events && events->hasEvent(cond.eventName);
    }

    static bool checkCondition(entt::registry& reg, entt::entity entity, FSMComponent& fsm,
                                const FSMEventComponent* events, const CustomCondition& cond) {
        return cond.predicate && cond.predicate(reg, entity);
    }

    static bool checkCondition(entt::registry& reg, entt::entity entity, FSMComponent& fsm,
                                const FSMEventComponent* events, const ActionsCompleteCondition& cond) {
        // Actions are complete when we're in InState phase (enter actions done)
        // and no ActionQueueComponent is running
        return !reg.any_of<ActionQueueComponent>(entity) ||
               reg.get<ActionQueueComponent>(entity).state == TimelineState::Completed;
    }

    static bool checkActionsComplete(entt::registry& reg, entt::entity entity) {
        // Check if ActionQueueComponent with fsm tag is done
        if (auto* cutscene = reg.try_get<ActionQueueComponent>(entity)) {
            if (cutscene->tag == "fsm_enter" || cutscene->tag == "fsm_exit") {
                return cutscene->state == TimelineState::Completed || cutscene->isComplete();
            }
        }
        // No cutscene means actions are complete (or there were none)
        return !reg.any_of<FSMActionsRunningTag>(entity);
    }

    static void emplaceActions(entt::registry& reg, entt::entity entity,
                                const std::vector<TimelineAction>& actions,
                                const std::string& tag) {
        // Deep copy actions since FSMState owns the originals
        auto& cutscene = reg.emplace_or_replace<ActionQueueComponent>(entity);
        cutscene.actions = actions;  // Copy
        cutscene.tag = tag;
        cutscene.currentActionIndex = 0;
        cutscene.state = TimelineState::Idle;
        cutscene.autoDestroy = false;  // FSMSystem manages lifecycle

        // Reset action states
        for (auto& action : cutscene.actions) {
            action.started = false;
            action.completed = false;
            action.elapsed = 0.0f;
        }

        cutscene.play();
    }

    static void clearEvents(entt::registry& reg) {
        auto view = reg.view<FSMEventComponent>();
        for (auto entity : view) {
            view.get<FSMEventComponent>(entity).clear();
        }
    }
};

// ============================================================
// Helper to create common FSM patterns
// ============================================================

namespace FSMPatterns {

    // Create a simple patrol FSM: Idle <-> Walk
    inline FSMComponent createPatrolFSM(entt::entity self,
                                         const glm::vec3& posA,
                                         const glm::vec3& posB,
                                         float walkDuration = 2.0f,
                                         float waitDuration = 1.0f) {
        using namespace AnimationBuilder;

        return FSMBuilder()
            .state("WaitA")
                .enter({ wait(waitDuration) })
                .transitionOnComplete("WalkToB")
            .state("WalkToB")
                .enter({ moveTo(self, posB, walkDuration, Easing::InOutQuad) })
                .transitionOnComplete("WaitB")
            .state("WaitB")
                .enter({ wait(waitDuration) })
                .transitionOnComplete("WalkToA")
            .state("WalkToA")
                .enter({ moveTo(self, posA, walkDuration, Easing::InOutQuad) })
                .transitionOnComplete("WaitA")
            .initialState("WaitA")
            .build();
    }

    // Create a trigger-based FSM: Idle -> Triggered -> Cooldown -> Idle
    inline FSMComponent createTriggerFSM(entt::entity self,
                                          std::vector<TimelineAction> onTriggerActions,
                                          float cooldownDuration = 3.0f) {
        using namespace AnimationBuilder;

        return FSMBuilder()
            .state("Idle")
                .transitionTo("Triggered", "trigger")
            .state("Triggered")
                .enter(std::move(onTriggerActions))
                .transitionOnComplete("Cooldown")
            .state("Cooldown")
                .enter({ wait(cooldownDuration) })
                .transitionOnComplete("Idle")
            .initialState("Idle")
            .build();
    }

    // Create an interaction FSM: Idle <-> Active (toggle on event)
    inline FSMComponent createToggleFSM(entt::entity self,
                                         std::vector<TimelineAction> onActivate,
                                         std::vector<TimelineAction> onDeactivate) {
        return FSMBuilder()
            .state("Inactive")
                .enter(std::move(onDeactivate))
                .transitionTo("Active", "toggle")
            .state("Active")
                .enter(std::move(onActivate))
                .transitionTo("Inactive", "toggle")
            .initialState("Inactive")
            .build();
    }

}  // namespace FSMPatterns
