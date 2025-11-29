#ifndef ENGINE_CORE_HPP
#define ENGINE_CORE_HPP

#include "task_scheduler.hpp"
#include "resource_manager.hpp"
#include "action_manager.hpp"
#include "input_manager.hpp"
#include <memory>

namespace Vapor {

/**
 * Central engine core that manages core subsystems
 * Provides:
 * - Unified task scheduling (TaskScheduler)
 * - Resource management (ResourceManager)
 * - Action management (ActionManager)
 */
class EngineCore {
public:
    EngineCore();
    ~EngineCore();

    // Initialize the engine with specified thread count
    void init(uint32_t numThreads = 0);

    // Shutdown and cleanup
    void shutdown();

    // Get the task scheduler
    TaskScheduler& getTaskScheduler() { return *m_taskScheduler; }

    // Get the resource manager
    ResourceManager& getResourceManager() { return *m_resourceManager; }

    // Get the action manager
    ActionManager& getActionManager() { return *m_actionManager; }

    // Get the input manager
    InputManager& getInputManager() { return *m_inputManager; }

    // Update per-frame (for async task management and action updates)
    void update(float deltaTime);

    // Check if engine is initialized
    bool isInitialized() const { return m_initialized; }

    // Get singleton instance
    static EngineCore* Get() { return s_instance; }

private:
    static EngineCore* s_instance;

    std::unique_ptr<TaskScheduler> m_taskScheduler;
    std::unique_ptr<ResourceManager> m_resourceManager;
    std::unique_ptr<ActionManager> m_actionManager;
    std::unique_ptr<InputManager> m_inputManager;

    bool m_initialized{false};
    uint32_t m_numThreads{0};
};

} // namespace Vapor

#endif // ENGINE_CORE_HPP
