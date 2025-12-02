#pragma once

#include "action_manager.hpp"
#include "audio_engine.hpp"
#include "input_manager.hpp"
#include "resource_manager.hpp"
#include "task_scheduler.hpp"
#include <memory>

namespace Vapor {

    class RmlUiManager;

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
            return *_taskScheduler;
        }

        // Get the resource manager
        ResourceManager& getResourceManager() {
            return *_resourceManager;
        }

        // Get the action manager
        ActionManager& getActionManager() {
            return *_actionManager;
        }

        // Get the input manager
        InputManager& getInputManager() {
            return *_inputManager;
        }

        // Get the audio manager
        AudioManager& getAudioManager() {
            return *_audioManager;
        }

        // Initialize RmlUI
        bool initRmlUI(int width, int height);

        // Get the RmlUI manager
        RmlUiManager* getRmlUiManager() {
            return _rmluiManager.get();
        }

        // Handle window resize for RmlUI
        void onRmlUIResize(int width, int height);

        // Process SDL event for RmlUI (returns true if event was consumed by RmlUI)
        bool processRmlUIEvent(const SDL_Event& event);

        // Update per-frame (for async task management and action updates)
        void update(float deltaTime);

        // Check if engine is initialized
        bool isInitialized() const {
            return _initialized;
        }

        // Get singleton instance
        static EngineCore* Get() {
            return s_instance;
        }

    private:
        static EngineCore* s_instance;

        std::unique_ptr<TaskScheduler> _taskScheduler;
        std::unique_ptr<ResourceManager> _resourceManager;
        std::unique_ptr<ActionManager> _actionManager;
        std::unique_ptr<InputManager> _inputManager;
        std::unique_ptr<AudioManager> _audioManager;
        std::unique_ptr<RmlUiManager> _rmluiManager;

        bool _initialized{ false };
        uint32_t _numThreads{ 0 };
    };

}// namespace Vapor