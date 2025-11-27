#pragma once

#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <string>
#include <cmath>

namespace Vapor {

// Forward declaration
class Action;

// ============================================================
// Easing Functions
// ============================================================

using EasingFunc = std::function<float(float)>;

namespace Easing {
    inline float Linear(float t) { return t; }

    inline float InQuad(float t) { return t * t; }
    inline float OutQuad(float t) { return t * (2.0f - t); }
    inline float InOutQuad(float t) {
        if (t < 0.5f) return 2.0f * t * t;
        return -1.0f + (4.0f - 2.0f * t) * t;
    }

    inline float InCubic(float t) { return t * t * t; }
    inline float OutCubic(float t) {
        float f = t - 1.0f;
        return f * f * f + 1.0f;
    }
    inline float InOutCubic(float t) {
        if (t < 0.5f) return 4.0f * t * t * t;
        float f = (2.0f * t - 2.0f);
        return 0.5f * f * f * f + 1.0f;
    }

    inline float OutBack(float t) {
        const float c1 = 1.70158f;
        const float c3 = c1 + 1.0f;
        float f = t - 1.0f;
        return 1.0f + c3 * f * f * f + c1 * f * f;
    }
}

// ============================================================
// Timer
// ============================================================

/**
 * Simple timer helper for Actions.
 *
 * Optional helper class for actions that need time tracking.
 * Not all actions need a timer, so this is provided as a utility.
 */
class Timer {
public:
    explicit Timer(float duration = 0.0f)
        : m_duration(duration), m_elapsed(0.0f) {}

    void reset(float duration = -1.0f) {
        if (duration >= 0.0f) {
            m_duration = duration;
        }
        m_elapsed = 0.0f;
    }

    bool update(float dt) {
        if (m_elapsed < m_duration) {
            m_elapsed += dt;
            return m_elapsed >= m_duration;
        }
        return true;
    }

    bool isComplete() const {
        return m_elapsed >= m_duration;
    }

    float getProgress() const {
        if (m_duration <= 0.0f) return 1.0f;
        return std::min(m_elapsed / m_duration, 1.0f);
    }

    float getDuration() const { return m_duration; }
    float getElapsed() const { return m_elapsed; }

private:
    float m_duration;
    float m_elapsed;
};

// ============================================================
// Action Base Class
// ============================================================

/**
 * Base class for time-based actions.
 *
 * Extend this class to create custom actions that execute over time.
 * Set m_finished = true when the action is complete.
 *
 * Example:
 *     class WaitAction : public Action {
 *     public:
 *         WaitAction(float duration) : m_timer(duration) {}
 *
 *         void update(float dt) override {
 *             if (m_timer.update(dt)) {
 *                 m_finished = true;
 *             }
 *         }
 *     private:
 *         Timer m_timer;
 *     };
 */
class Action {
public:
    virtual ~Action() = default;

    // Called when action starts (override for initialization)
    virtual void onStart() {}

    // Update action state (override in subclasses)
    virtual void update(float dt) = 0;

    // Check if action has finished
    bool isDone() const { return m_finished; }

    // Mark as finished (can be called by subclasses)
    void finish() { m_finished = true; }

    // Reset the action (for reuse in RepeatAction, etc.)
    virtual void reset() {
        m_finished = false;
    }

protected:
    bool m_finished = false;

    friend class ActionManager;
};

// ============================================================
// Concrete Action Types
// ============================================================

/**
 * Wait for a duration.
 *
 * Example:
 *     auto delay = std::make_shared<DelayAction>(0.5f); // Wait 0.5 seconds
 */
class DelayAction : public Action {
public:
    explicit DelayAction(float duration) : m_timer(duration) {}

    void onStart() override {
        m_timer.reset();
    }

    void update(float dt) override {
        if (m_timer.update(dt)) {
            m_finished = true;
        }
    }

private:
    Timer m_timer;
};

/**
 * Execute a callback immediately.
 *
 * Example:
 *     auto callback = std::make_shared<CallbackAction>([]() {
 *         std::cout << "Action completed!" << std::endl;
 *     });
 */
class CallbackAction : public Action {
public:
    using Callback = std::function<void()>;

    explicit CallbackAction(Callback callback)
        : m_callback(std::move(callback)) {}

    void onStart() override {
        if (m_callback) {
            m_callback();
        }
        m_finished = true;
    }

    void update(float dt) override {
        // Already finished in onStart
    }

private:
    Callback m_callback;
};

/**
 * Execute a callback after a delay.
 *
 * Example:
 *     auto timedCallback = std::make_shared<TimedCallbackAction>(0.5f, []() {
 *         std::cout << "Half second passed!" << std::endl;
 *     });
 */
class TimedCallbackAction : public Action {
public:
    using Callback = std::function<void()>;

    TimedCallbackAction(float duration, Callback callback)
        : m_timer(duration), m_callback(std::move(callback)) {}

    void onStart() override {
        if (m_timer.getDuration() <= 0.0f) {
            if (m_callback) m_callback();
            m_finished = true;
        } else {
            m_timer.reset();
        }
    }

    void update(float dt) override {
        if (m_timer.update(dt)) {
            if (m_callback) m_callback();
            m_finished = true;
        }
    }

private:
    Timer m_timer;
    Callback m_callback;
};

/**
 * Update action that runs for a duration with progress callback.
 *
 * Example:
 *     auto update = std::make_shared<UpdateAction>(1.0f,
 *         [](float dt, float progress) {
 *             // Called every frame with dt and progress (0.0 to 1.0)
 *         });
 */
class UpdateAction : public Action {
public:
    using UpdateFunc = std::function<void(float dt, float progress)>;

    UpdateAction(float duration, UpdateFunc updateFunc)
        : m_timer(duration), m_updateFunc(std::move(updateFunc)) {}

    void onStart() override {
        m_timer.reset();
    }

    void update(float dt) override {
        if (m_timer.update(dt)) {
            m_finished = true;
        }
        if (m_updateFunc) {
            m_updateFunc(dt, m_timer.getProgress());
        }
    }

private:
    Timer m_timer;
    UpdateFunc m_updateFunc;
};

/**
 * Update action that runs forever until manually stopped.
 *
 * Example:
 *     auto forever = std::make_shared<UpdateForeverAction>(
 *         [](float dt) {
 *             // Called every frame forever
 *         });
 */
class UpdateForeverAction : public Action {
public:
    using UpdateFunc = std::function<void(float dt)>;

    explicit UpdateForeverAction(UpdateFunc updateFunc)
        : m_updateFunc(std::move(updateFunc)) {}

    void update(float dt) override {
        if (m_updateFunc) {
            m_updateFunc(dt);
        }
    }

private:
    UpdateFunc m_updateFunc;
};

/**
 * Sequence of actions executed in order.
 *
 * Each action starts after the previous one completes.
 *
 * Example:
 *     auto timeline = std::make_shared<TimelineAction>();
 *     timeline->add(std::make_shared<DelayAction>(1.0f));
 *     timeline->add(std::make_shared<CallbackAction>([]() { ... }));
 */
class TimelineAction : public Action {
public:
    TimelineAction() = default;

    TimelineAction& add(std::shared_ptr<Action> action) {
        m_actions.push_back(action);
        return *this;
    }

    void onStart() override {
        if (m_actions.empty()) {
            m_finished = true;
            return;
        }
        m_currentIndex = 0;
        startCurrentAction();
    }

    void update(float dt) override {
        if (m_currentIndex >= m_actions.size()) {
            m_finished = true;
            return;
        }

        auto& currentAction = m_actions[m_currentIndex];
        currentAction->update(dt);

        if (currentAction->isDone()) {
            m_currentIndex++;
            if (m_currentIndex >= m_actions.size()) {
                m_finished = true;
            } else {
                startCurrentAction();
            }
        }
    }

private:
    void startCurrentAction() {
        if (m_currentIndex < m_actions.size()) {
            m_actions[m_currentIndex]->onStart();
        }
    }

    std::vector<std::shared_ptr<Action>> m_actions;
    size_t m_currentIndex = 0;
};

/**
 * Execute multiple actions simultaneously.
 *
 * All actions start at the same time and run in parallel.
 * The parallel action completes when all child actions complete.
 *
 * Example:
 *     auto parallel = std::make_shared<ParallelAction>();
 *     parallel->add(std::make_shared<DelayAction>(1.0f));
 *     parallel->add(std::make_shared<UpdateAction>(...));
 */
class ParallelAction : public Action {
public:
    ParallelAction() = default;

    ParallelAction& add(std::shared_ptr<Action> action) {
        m_actions.push_back(action);
        return *this;
    }

    void onStart() override {
        if (m_actions.empty()) {
            m_finished = true;
            return;
        }

        for (auto& action : m_actions) {
            action->onStart();
        }
    }

    void update(float dt) override {
        bool allDone = true;
        for (auto& action : m_actions) {
            if (!action->isDone()) {
                action->update(dt);
                if (!action->isDone()) {
                    allDone = false;
                }
            }
        }

        if (allDone) {
            m_finished = true;
        }
    }

private:
    std::vector<std::shared_ptr<Action>> m_actions;
};

/**
 * Repeat an action multiple times or infinitely.
 *
 * Example:
 *     // Repeat 3 times
 *     auto repeat = std::make_shared<RepeatAction>(
 *         std::make_shared<DelayAction>(1.0f), 3);
 *
 *     // Repeat forever (-1)
 *     auto repeatForever = std::make_shared<RepeatAction>(
 *         std::make_shared<UpdateAction>(...), -1);
 */
class RepeatAction : public Action {
public:
    RepeatAction(std::shared_ptr<Action> action, int count = -1)
        : m_action(action), m_count(count) {}

    void onStart() override {
        m_currentCount = 0;
        if (m_action) {
            m_action->onStart();
        }
    }

    void update(float dt) override {
        if (!m_action) {
            m_finished = true;
            return;
        }

        m_action->update(dt);

        if (m_action->isDone()) {
            m_currentCount++;

            if (m_count == -1 || m_currentCount < m_count) {
                // Reset the action for next iteration
                m_action->reset();
                m_action->onStart();
            } else {
                m_finished = true;
            }
        }
    }

private:
    std::shared_ptr<Action> m_action;
    int m_count;
    int m_currentCount = 0;
};

// ============================================================
// ActionManager
// ============================================================

/**
 * Manages a collection of actions and updates them each frame.
 *
 * Automatically removes actions when they complete. Useful for managing
 * timed operations, animations, and delayed behaviors.
 *
 * Supports tagging actions for grouped management (e.g., stop all animations
 * when state changes).
 *
 * Example:
 *     ActionManager manager;
 *
 *     // Start an action
 *     auto action = std::make_shared<DelayAction>(2.0f);
 *     manager.start(action);
 *
 *     // With tags
 *     manager.start(animAction, "idle_anim");
 *     manager.stopByTag("idle_anim"); // Stop all actions with this tag
 *
 *     // In game loop:
 *     manager.update(dt);
 */
class ActionManager {
public:
    ActionManager() = default;
    ~ActionManager() = default;

    // Add an action to be managed and updated
    std::shared_ptr<Action> start(std::shared_ptr<Action> action, const std::string& tag = "");

    // Stop and remove a specific action
    void stop(const std::shared_ptr<Action>& action);

    // Stop and remove all actions with the specified tag
    void stopByTag(const std::string& tag);

    // Remove all active actions
    void stopAll();

    // Check if any actions with the specified tag are active
    bool hasTag(const std::string& tag) const;

    // Get all active actions with the specified tag
    std::vector<std::shared_ptr<Action>> getActionsByTag(const std::string& tag) const;

    // Get total number of active actions
    size_t getActionCount() const { return m_actions.size(); }

    // Update all active actions and remove completed ones
    void update(float dt);

private:
    void removeAction(const std::shared_ptr<Action>& action);

    std::vector<std::shared_ptr<Action>> m_actions;
    std::unordered_map<std::shared_ptr<Action>, std::unordered_set<std::string>> m_actionTags;
    std::unordered_map<std::string, std::unordered_set<std::shared_ptr<Action>>> m_tagActions;
};

} // namespace Vapor
