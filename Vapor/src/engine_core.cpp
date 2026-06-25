#include "engine_core.hpp"
#include "file_system.hpp"
#include "renderer.hpp"
#include "video_recorder.hpp"
#include "rmlui_manager.hpp"
#include "imgui.h"
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

        // Initialize file system search paths before any asset loading
        FileSystem::instance().initialize();

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

        // Initialize video recorder and wire it to the audio manager so
        // recordings automatically include the engine's mixed audio output.
        _videoRecorder = std::make_unique<VideoRecorder>();
        _videoRecorder->setAudioManager(_audioManager.get());

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

        // Stop any in-progress recording before teardown.
        if (_videoRecorder && _videoRecorder->isRecording())
            _videoRecorder->stopRecording();
        _videoRecorder.reset();

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
        _taskScheduler->processMainThreadTasks();
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

    void EngineCore::attachRenderer(Renderer* renderer, const std::string& outputBasePath) {
        _videoRecorder->setBaseOutputDir(outputBasePath);

        renderer->setEngineWindowCallback([this, renderer]() {
            if (_videoRecorder->isRecording())
                _videoRecorder->captureFrame();
#ifndef NDEBUG
            if (ImGui::CollapsingHeader("Recording", ImGuiTreeNodeFlags_DefaultOpen))
                _videoRecorder->drawImGui(*renderer);
#endif
        });
    }

    VideoRecorder& EngineCore::getVideoRecorder() {
        return *_videoRecorder;
    }

    auto EngineCore::initRmlUI(int width, int height) -> bool {
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

    auto EngineCore::processRmlUIEvent(const SDL_Event& event) -> bool {
        if (_rmluiManager && _rmluiManager->IsInitialized()) {
            return _rmluiManager->ProcessEvent(event);
        }
        return false;
    }

}// namespace Vapor
