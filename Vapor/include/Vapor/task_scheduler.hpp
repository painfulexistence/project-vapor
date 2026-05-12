#pragma once

#include <atomic>
#include <enkiTS/TaskScheduler.h>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace Vapor {

    /**
     * Wrapper around enkiTS task scheduler for async resource loading
     * Provides a simplified interface for managing concurrent tasks
     */
    class TaskScheduler {
    public:
        TaskScheduler();
        ~TaskScheduler();

        // Initialize the task scheduler with specified number of threads
        // If numThreads is 0, uses hardware concurrency
        void init(uint32_t numThreads = 0);

        // Shutdown the task scheduler and wait for all tasks to complete
        void shutdown();

        // Get the underlying enkiTS task scheduler
        enki::TaskScheduler* getScheduler() {
            return m_scheduler.get();
        }

        // Wait for all submitted tasks to complete
        void waitForAll();

        // Submit a lambda function as a task
        template<typename Func> void submitTask(Func&& func);

        // Check if scheduler is initialized

        // Submit a task to be executed on the Main Thread (during processMainThreadTasks)
        template<typename Func> void runOnMainThread(Func&& func) {
            std::lock_guard<std::mutex> lock(m_mainThreadMutex);
            m_mainThreadQueue.push_back(std::forward<Func>(func));
        }


        // Process all pending main thread tasks (should be called from the main thread)
        void processMainThreadTasks();

        bool isInitialized() const {
            return m_initialized;
        }

    private:
        std::vector<std::function<void()>> m_mainThreadQueue;
        std::mutex m_mainThreadMutex;

        std::unique_ptr<enki::TaskScheduler> m_scheduler;
        std::atomic<bool> m_initialized{ false };
    };

    // Lambda task wrapper for enkiTS
    class LambdaTask : public enki::ITaskSet {
    public:
        LambdaTask(std::function<void()> func) : m_func(std::move(func)) {
        }

        void ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum) override {
            m_func();
        }

    private:
        std::function<void()> m_func;
    };

    template<typename Func> void TaskScheduler::submitTask(Func&& func) {
        if (!m_initialized) {
            // If not initialized, run synchronously
            func();
            return;
        }

        auto task = new LambdaTask(std::forward<Func>(func));
        m_scheduler->AddTaskSetToPipe(task);
    }

}// namespace Vapor
