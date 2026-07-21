#include "task_scheduler.hpp"
#include <thread>
#include <tracy/Tracy.hpp>

using namespace Vapor;

namespace Vapor {

    TaskScheduler::TaskScheduler() {
        m_scheduler = std::make_unique<enki::TaskScheduler>();
    }

    TaskScheduler::~TaskScheduler() {
        if (m_initialized) {
            shutdown();
        }
    }

    void TaskScheduler::init(uint32_t numThreads) {
        ZoneScoped;

        if (m_initialized) {
            return;
        }

        // If numThreads is 0, use hardware concurrency
        if (numThreads == 0) {
            numThreads = std::thread::hardware_concurrency();
            if (numThreads == 0) {
                numThreads = 4;// Fallback to 4 threads
            }
        }

        m_scheduler->Initialize(numThreads);
        m_initialized = true;
    }

    void TaskScheduler::shutdown() {
        if (!m_initialized) {
            return;
        }

        waitForAll();
        m_initialized = false;
    }

    void TaskScheduler::waitForAll() {
        ZoneScoped;

        if (!m_initialized) {
            return;
        }

        m_scheduler->WaitforAll();
    }

    void TaskScheduler::processMainThreadTasks() {
        std::vector<std::function<void()>> tasksToExecute;
        {
            std::lock_guard<std::mutex> lock(m_mainThreadMutex);
            tasksToExecute.swap(m_mainThreadQueue);
        }

        for (auto& task : tasksToExecute) {
            task();
        }
    }

}// namespace Vapor
