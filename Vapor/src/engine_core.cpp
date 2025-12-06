#include "engine_core.hpp"
#include "rmlui_manager.hpp"
#include <fmt/core.h>
#include <fmt/format.h>
#include <thread>
#include <tracy/Tracy.hpp>

namespace Vapor {

    EngineCore* EngineCore::s_instance = nullptr;

    EngineCore::EngineCore() {
        if (s_instance != nullptr) {
            fmt::print("Warning: Multiple EngineCore instances created\n");
        }
        s_instance = this;
    }

    EngineCore::~EngineCore() {
        if (_initialized) {
            shutdown();
        }

        if (s_instance == this) {
            s_instance = nullptr;
        }
    }

    void EngineCore::init(uint32_t numThreads) {
        ZoneScoped;

        if (_initialized) {
            fmt::print("EngineCore already initialized\n");
            return;
        }

        // Determine thread count
        if (numThreads == 0) {
            numThreads = std::thread::hardware_concurrency();
            if (numThreads == 0) {
                numThreads = 4;// Fallback
            }
        }
        _numThreads = numThreads;

        fmt::print("Initializing EngineCore with {} threads\n", _numThreads);

        // Initialize task scheduler
        _taskScheduler = std::make_unique<TaskScheduler>();
        _taskScheduler->init(_numThreads);

        // Initialize resource manager
        _resourceManager = std::make_unique<ResourceManager>(*_taskScheduler);

        // Initialize action manager
        _actionManager = std::make_unique<ActionManager>();

        // Initialize input manager
        _inputManager = std::make_unique<InputManager>();

        // Initialize audio manager
        _audioManager = std::make_unique<AudioManager>();
        _audioManager->init();

        _initialized = true;

        fmt::print("EngineCore initialized successfully\n");
    }

    void EngineCore::shutdown() {
        ZoneScoped;

        if (!_initialized) {
            return;
        }

        fmt::print("Shutting down EngineCore...\n");

        // Wait for all pending tasks
        _taskScheduler->waitForAll();

        _actionManager->stopAll();

        // Cleanup subsystems in reverse order
        if (_rmluiManager) {
            _rmluiManager->Shutdown();
            _rmluiManager.reset();
        }
        _audioManager->shutdown();
        _audioManager.reset();
        _inputManager.reset();
        _actionManager.reset();
        _resourceManager.reset();
        _taskScheduler->shutdown();
        _taskScheduler.reset();

        _initialized = false;

        fmt::print("EngineCore shutdown complete\n");
    }

    void EngineCore::update(float deltaTime) {
        ZoneScoped;

        if (!_initialized) {
            return;
        }

        // Update action manager (time-based actions)
        _actionManager->update(deltaTime);

        // Update audio manager (cleanup finished sounds, invoke callbacks)
        _audioManager->update(deltaTime);

        // Update RmlUI
        if (_rmluiManager) {
            _rmluiManager->Update(deltaTime);
        }

        // Future: Handle async task completion callbacks
        // Future: Manage render command buffer submission
        // Future: Coordinate physics-render synchronization
    }

    bool EngineCore::initRmlUI(int width, int height) {
        if (_rmluiManager) {
            fmt::print("RmlUI already initialized\n");
            return true;
        }

        _rmluiManager = std::make_unique<RmlUiManager>();
        if (!_rmluiManager->Initialize(width, height)) {
            fmt::print("Failed to initialize RmlUI\n");
            _rmluiManager.reset();
            return false;
        }

        fmt::print("RmlUI initialized successfully ({}x{})\n", width, height);
        return true;
    }

    void EngineCore::onRmlUIResize(int width, int height) {
        if (_rmluiManager) {
            _rmluiManager->OnResize(width, height);
        }
    }

    bool EngineCore::processRmlUIEvent(const SDL_Event& event) {
        if (_rmluiManager && _rmluiManager->IsInitialized()) {
            return _rmluiManager->ProcessEvent(event);
        }
        return false;
    }

}// namespace Vapor
