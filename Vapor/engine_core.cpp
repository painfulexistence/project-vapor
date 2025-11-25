#include "engine_core.hpp"
#include <fmt/core.h>
#include <thread>

namespace Vapor {

EngineCore* EngineCore::s_instance = nullptr;

EngineCore::EngineCore() {
    if (s_instance != nullptr) {
        fmt::print("Warning: Multiple EngineCore instances created\n");
    }
    s_instance = this;
}

EngineCore::~EngineCore() {
    if (m_initialized) {
        shutdown();
    }

    if (s_instance == this) {
        s_instance = nullptr;
    }
}

void EngineCore::init(uint32_t numThreads) {
    if (m_initialized) {
        fmt::print("EngineCore already initialized\n");
        return;
    }

    // Determine thread count
    if (numThreads == 0) {
        numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0) {
            numThreads = 4; // Fallback
        }
    }
    m_numThreads = numThreads;

    fmt::print("Initializing EngineCore with {} threads\n", m_numThreads);

    // Initialize task scheduler
    m_taskScheduler = std::make_unique<TaskScheduler>();
    m_taskScheduler->init(m_numThreads);

    // Initialize async asset loader
    m_assetLoader = std::make_unique<AsyncAssetLoader>(*m_taskScheduler);

    // Initialize Jolt job system with enkiTS
    m_joltJobSystem = std::make_unique<JoltEnkiJobSystem>(*m_taskScheduler, 2048);

    m_initialized = true;

    fmt::print("EngineCore initialized successfully\n");
}

void EngineCore::shutdown() {
    if (!m_initialized) {
        return;
    }

    fmt::print("Shutting down EngineCore...\n");

    // Wait for all pending tasks
    waitForAllTasks();

    // Cleanup subsystems in reverse order
    m_joltJobSystem.reset();
    m_assetLoader.reset();
    m_taskScheduler->shutdown();
    m_taskScheduler.reset();

    m_initialized = false;

    fmt::print("EngineCore shutdown complete\n");
}

void EngineCore::waitForAllTasks() {
    if (!m_initialized) {
        return;
    }

    m_taskScheduler->waitForAll();
}

void EngineCore::update(float deltaTime) {
    if (!m_initialized) {
        return;
    }

    // Future: Handle async task completion callbacks
    // Future: Manage render command buffer submission
    // Future: Coordinate physics-render synchronization
}

} // namespace Vapor
