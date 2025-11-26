#ifndef ENGINE_CORE_HPP
#define ENGINE_CORE_HPP

#include "task_scheduler.hpp"
#include "resource_manager.hpp"
#include <memory>

namespace Vapor {

/**
 * Central engine core that manages core subsystems
 * Provides:
 * - Unified task scheduling (TaskScheduler)
 * - Resource management (ResourceManager)
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

    // Update per-frame (for async task management)
    void update(float deltaTime);

    // Check if engine is initialized
    bool isInitialized() const { return m_initialized; }

    // Get singleton instance
    static EngineCore* Get() { return s_instance; }

private:
    static EngineCore* s_instance;

    std::unique_ptr<TaskScheduler> m_taskScheduler;
    std::unique_ptr<ResourceManager> m_resourceManager;

    bool m_initialized{false};
    uint32_t m_numThreads{0};
};

} // namespace Vapor

#endif // ENGINE_CORE_HPP
