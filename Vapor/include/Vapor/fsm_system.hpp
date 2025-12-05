#pragma once

#include "fsm.hpp"
#include "action_manager.hpp"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace Vapor {

// ============================================================
// FSM System - Updates all FSM components in the registry
// ============================================================

/**
 * Updates all entities with FSMComponent.
 *
 * Call this in your game loop:
 *     updateFSMSystem(registry, deltaTime);
 */
inline void updateFSMSystem(entt::registry& registry, float deltaTime) {
    auto view = registry.view<FSMComponent>();
    for (auto entity : view) {
        auto& fsmComp = view.get<FSMComponent>(entity);
        if (fsmComp.fsm && fsmComp.fsm->isRunning()) {
            fsmComp.fsm->update(deltaTime);
        }
    }
}

// ============================================================
// Action-Integrated States
// ============================================================

/**
 * A state that can trigger actions via ActionManager.
 *
 * Example:
 *     class AttackState : public ActionState {
 *     public:
 *         AttackState(ActionManager& am) : ActionState("Attack", am) {}
 *
 *         void onEnter() override {
 *             // Play attack animation (1 second)
 *             runAction(std::make_shared<TimedCallbackAction>(1.0f, [this]() {
 *                 sendEvent("AttackComplete");
 *             }));
 *         }
 *     };
 */
class ActionState : public FSMState {
public:
    ActionState(std::string name, ActionManager& actionManager)
        : FSMState(std::move(name))
        , m_actionManager(actionManager) {}

    void onExit() override {
        // Stop all actions associated with this state when exiting
        stopAllActions();
    }

    // Run an action with automatic cleanup on state exit
    std::shared_ptr<Action> runAction(std::shared_ptr<Action> action) {
        std::string tag = getActionTag();
        return m_actionManager.start(std::move(action), tag);
    }

    // Stop all actions started by this state
    void stopAllActions() {
        m_actionManager.stopByTag(getActionTag());
    }

    // Check if this state has any running actions
    bool hasRunningActions() const {
        return m_actionManager.hasTag(getActionTag());
    }

protected:
    ActionManager& m_actionManager;

    std::string getActionTag() const {
        return "fsm_state_" + m_name;
    }
};

/**
 * A state that runs for a duration using ActionManager, then transitions.
 */
class ActionTimedState : public ActionState {
public:
    ActionTimedState(std::string name, ActionManager& actionManager,
                     float duration, std::string nextState)
        : ActionState(std::move(name), actionManager)
        , m_duration(duration)
        , m_nextState(std::move(nextState)) {
        addTransition(m_nextState, "_ActionTimedComplete");
    }

    void onEnter() override {
        runAction(std::make_shared<TimedCallbackAction>(m_duration, [this]() {
            sendEvent("_ActionTimedComplete");
        }));
    }

protected:
    float m_duration;
    std::string m_nextState;
};

/**
 * A state that plays an animation-like sequence of callbacks over time.
 */
class ActionSequenceState : public ActionState {
public:
    using Callback = std::function<void()>;

    ActionSequenceState(std::string name, ActionManager& actionManager, std::string nextState)
        : ActionState(std::move(name), actionManager)
        , m_nextState(std::move(nextState)) {
        addTransition(m_nextState, "_SequenceComplete");
    }

    // Add a delay step
    ActionSequenceState& delay(float duration) {
        m_steps.push_back([duration](TimelineAction& timeline) {
            timeline.add(std::make_shared<DelayAction>(duration));
        });
        return *this;
    }

    // Add a callback step
    ActionSequenceState& callback(Callback cb) {
        m_steps.push_back([cb = std::move(cb)](TimelineAction& timeline) {
            timeline.add(std::make_shared<CallbackAction>(cb));
        });
        return *this;
    }

    // Add a timed update step
    ActionSequenceState& update(float duration, std::function<void(float, float)> updateFunc) {
        m_steps.push_back([duration, updateFunc = std::move(updateFunc)](TimelineAction& timeline) {
            timeline.add(std::make_shared<UpdateAction>(duration, updateFunc));
        });
        return *this;
    }

    void onEnter() override {
        auto timeline = std::make_shared<TimelineAction>();

        for (auto& step : m_steps) {
            step(*timeline);
        }

        // Add completion callback
        timeline->add(std::make_shared<CallbackAction>([this]() {
            sendEvent("_SequenceComplete");
        }));

        runAction(timeline);
    }

protected:
    std::string m_nextState;
    std::vector<std::function<void(TimelineAction&)>> m_steps;
};

// ============================================================
// FSM Event Broadcasting
// ============================================================

/**
 * Broadcast an event to all FSM components in the registry.
 */
inline void broadcastFSMEvent(entt::registry& registry, const FSMEvent& event) {
    auto view = registry.view<FSMComponent>();
    for (auto entity : view) {
        auto& fsmComp = view.get<FSMComponent>(entity);
        if (fsmComp.fsm) {
            fsmComp.fsm->sendEvent(event);
        }
    }
}

inline void broadcastFSMEvent(entt::registry& registry, const std::string& eventName) {
    broadcastFSMEvent(registry, FSMEvent(eventName));
}

/**
 * Send an event to a specific entity's FSM.
 */
inline void sendFSMEvent(entt::registry& registry, entt::entity entity, const FSMEvent& event) {
    if (auto* fsmComp = registry.try_get<FSMComponent>(entity)) {
        if (fsmComp->fsm) {
            fsmComp->fsm->sendEvent(event);
        }
    }
}

inline void sendFSMEvent(entt::registry& registry, entt::entity entity, const std::string& eventName) {
    sendFSMEvent(registry, entity, FSMEvent(eventName));
}

// ============================================================
// FSM Debug Utilities
// ============================================================

/**
 * Get debug information about all FSMs in the registry.
 */
struct FSMDebugInfo {
    entt::entity entity;
    std::string currentState;
    bool isRunning;
};

inline std::vector<FSMDebugInfo> getFSMDebugInfo(entt::registry& registry) {
    std::vector<FSMDebugInfo> info;
    auto view = registry.view<FSMComponent>();
    for (auto entity : view) {
        auto& fsmComp = view.get<FSMComponent>(entity);
        FSMDebugInfo debug;
        debug.entity = entity;
        debug.isRunning = fsmComp.fsm && fsmComp.fsm->isRunning();
        debug.currentState = debug.isRunning ? fsmComp.fsm->getCurrentStateName() : "";
        info.push_back(debug);
    }
    return info;
}

// ============================================================
// Common Game States
// ============================================================

/**
 * A patrol state that cycles through waypoints.
 * Sends "WaypointReached" event when reaching each waypoint.
 */
template<typename PositionGetter, typename PositionSetter>
class PatrolState : public FSMState {
public:
    PatrolState(std::string name,
                std::vector<glm::vec3> waypoints,
                float speed,
                float arrivalThreshold,
                PositionGetter getPos,
                PositionSetter setPos)
        : FSMState(std::move(name))
        , m_waypoints(std::move(waypoints))
        , m_speed(speed)
        , m_arrivalThreshold(arrivalThreshold)
        , m_getPosition(std::move(getPos))
        , m_setPosition(std::move(setPos)) {}

    void onEnter() override {
        m_currentWaypoint = 0;
    }

    void onUpdate(float dt) override {
        if (m_waypoints.empty()) return;

        glm::vec3 currentPos = m_getPosition();
        glm::vec3 targetPos = m_waypoints[m_currentWaypoint];
        glm::vec3 direction = targetPos - currentPos;
        float distance = glm::length(direction);

        if (distance < m_arrivalThreshold) {
            // Reached waypoint
            m_currentWaypoint = (m_currentWaypoint + 1) % m_waypoints.size();
            sendEvent("WaypointReached");
        } else {
            // Move towards waypoint
            glm::vec3 velocity = glm::normalize(direction) * m_speed * dt;
            m_setPosition(currentPos + velocity);
        }
    }

    size_t getCurrentWaypointIndex() const { return m_currentWaypoint; }

protected:
    std::vector<glm::vec3> m_waypoints;
    float m_speed;
    float m_arrivalThreshold;
    size_t m_currentWaypoint = 0;
    PositionGetter m_getPosition;
    PositionSetter m_setPosition;
};

/**
 * An idle state that waits for a random duration before transitioning.
 */
class RandomIdleState : public FSMState {
public:
    RandomIdleState(std::string name, float minDuration, float maxDuration, std::string nextState)
        : FSMState(std::move(name))
        , m_minDuration(minDuration)
        , m_maxDuration(maxDuration)
        , m_nextState(std::move(nextState)) {
        addTransition(m_nextState, "_IdleComplete");
    }

    void onEnter() override {
        // Random duration between min and max
        float range = m_maxDuration - m_minDuration;
        m_duration = m_minDuration + (static_cast<float>(rand()) / RAND_MAX) * range;
        m_elapsed = 0.0f;
    }

    void onUpdate(float dt) override {
        m_elapsed += dt;
        if (m_elapsed >= m_duration) {
            sendEvent("_IdleComplete");
        }
    }

protected:
    float m_minDuration;
    float m_maxDuration;
    float m_duration = 0.0f;
    float m_elapsed = 0.0f;
    std::string m_nextState;
};

} // namespace Vapor
