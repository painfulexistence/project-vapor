#ifndef ENGINE_CORE_HPP
#define ENGINE_CORE_HPP

#include "action_manager.hpp"
#include "audio_engine.hpp"
#include "input_manager.hpp"
#include "resource_manager.hpp"
#include "task_scheduler.hpp"
#include <memory>

// Forward declarations
namespace Vapor {
    class RmlUiManager;
}
namespace MTL {
    class Device;
}

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
        TaskScheduler& getTaskScheduler() {
            return *m_taskScheduler;
        }

        // Get the resource manager
        ResourceManager& getResourceManager() {
            return *m_resourceManager;
        }

        // Get the action manager
        ActionManager& getActionManager() {
            return *m_actionManager;
        }

        // Get the input manager
        InputManager& getInputManager() {
            return *m_inputManager;
        }

        // Get the audio manager
        AudioManager& getAudioManager() {
            return *m_audioManager;
        }

        // Initialize RmlUI
        bool initRmlUI(int width, int height);

        // Get the RmlUI manager
        RmlUiManager* getRmlUiManager() {
            return m_rmluiManager.get();
        }

        // Handle window resize for RmlUI
        void onRmlUIResize(int width, int height);

        // Process SDL event for RmlUI (returns true if event was consumed by RmlUI)
        bool processRmlUIEvent(const SDL_Event& event);

        // Update per-frame (for async task management and action updates)
        void update(float deltaTime);

        // Check if engine is initialized
        bool isInitialized() const {
            return m_initialized;
        }

        // Get singleton instance
        static EngineCore* Get() {
            return s_instance;
        }

    private:
        static EngineCore* s_instance;

        std::unique_ptr<TaskScheduler> m_taskScheduler;
        std::unique_ptr<ResourceManager> m_resourceManager;
        std::unique_ptr<ActionManager> m_actionManager;
        std::unique_ptr<InputManager> m_inputManager;
        std::unique_ptr<AudioManager> m_audioManager;
        std::unique_ptr<RmlUiManager> m_rmluiManager;

        bool m_initialized{ false };
        uint32_t m_numThreads{ 0 };
    };

}// namespace Vapor

#endif// ENGINE_CORE_HPP
