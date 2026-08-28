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

        // Submitted lambda tasks. enkiTS never owns task objects, so each one
        // must live somewhere until it completes; they park here and are
        // reaped lazily (see submitTask) or in waitForAll. The old code new'd
        // them and forgot them, which leaked not just the wrapper but
        // everything its closure captured — a generation job pins its whole
        // VoxelWorld, a collider bake its shapes. Declared BEFORE m_scheduler:
        // members die in reverse order, and the scheduler's destructor (which
        // waits out the queue) must run while these objects still exist.
        class LambdaTask;
        std::vector<std::unique_ptr<LambdaTask>> m_tasks;
        std::mutex m_taskMutex;

        std::unique_ptr<enki::TaskScheduler> m_scheduler;
        std::atomic<bool> m_initialized{ false };
    };

    // Lambda task wrapper for enkiTS
    class TaskScheduler::LambdaTask : public enki::ITaskSet {
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

        std::lock_guard<std::mutex> lock(m_taskMutex);
        // Reap what has finished before adding more — amortized cleanup with
        // no per-frame hook. GetIsComplete() true means the scheduler is done
        // touching the object, so deleting it here is safe.
        std::erase_if(m_tasks, [](const std::unique_ptr<LambdaTask>& t) { return t->GetIsComplete(); });
        m_tasks.push_back(std::make_unique<LambdaTask>(std::forward<Func>(func)));
        m_scheduler->AddTaskSetToPipe(m_tasks.back().get());
    }

}// namespace Vapor
