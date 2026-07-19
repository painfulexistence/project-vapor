#include "jolt_enki_job_system.hpp"
#include <fmt/core.h>
#include <thread>

using namespace Vapor;

namespace Vapor {

    JoltEnkiJobSystem::JoltEnkiJobSystem(TaskScheduler& scheduler, uint32_t maxJobs)
      : JPH::JobSystemWithBarrier(256)// Initialize with max barriers
        ,
        m_scheduler(scheduler), m_maxJobs(maxJobs) {

        fmt::print("JoltEnkiJobSystem: Initialized with max {} jobs\n", maxJobs);
    }

    JoltEnkiJobSystem::~JoltEnkiJobSystem() {
        // Wait for all jobs to complete
        m_scheduler.waitForAll();
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    auto JoltEnkiJobSystem::GetMaxConcurrency() const -> int {
        // Return hardware thread count
        int concurrency = std::thread::hardware_concurrency();
        return concurrency > 0 ? concurrency : 4;
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    auto JoltEnkiJobSystem::CreateJob(
        const char* name, JPH::ColorArg color, const JPH::JobSystem::JobFunction& jobFunction, uint32_t numDependencies
    ) -> JPH::JobHandle {
        // Check if we have available job slots
        while (m_numJobs.load() >= m_maxJobs) {
            std::this_thread::yield();
        }

        // Create a new job using the protected Job constructor
        Job* job = new Job(name, color, this, jobFunction, numDependencies);

        // Construct the handle BEFORE queueing (JobSystemThreadPool order): a
        // fast worker can Execute+Release the queue's reference before we return.
        JPH::JobHandle handle(job);

        // If no dependencies, queue it immediately
        if (numDependencies == 0) {
            QueueJob(job);
        }

        m_numJobs++;

        return handle;
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    void JoltEnkiJobSystem::QueueJob(JPH::JobSystem::Job* job) {
        if (!job) return;

        // Queue's reference, released after Execute in ExecuteRange
        // (JobSystemThreadPool semantics): keeps the job alive if the caller
        // drops its handle before execution.
        job->AddRef();

        // Reuse a completed pooled task or grow the pool. The lock must span
        // both the pick and AddTaskSetToPipe — the pipe call is what flips
        // the task to not-complete.
        std::lock_guard<std::mutex> lock(m_taskPoolMutex);
        JoltJobTask* task = nullptr;
        for (auto& t : m_taskPool) {
            if (t->GetIsComplete()) {
                task = t.get();
                break;
            }
        }
        if (!task) {
            m_taskPool.push_back(std::make_unique<JoltJobTask>());
            task = m_taskPool.back().get();
        }
        task->setJob(job);
        m_scheduler.getScheduler()->AddTaskSetToPipe(task);
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    void JoltEnkiJobSystem::QueueJobs(JPH::JobSystem::Job** jobs, uint32_t numJobs) {
        for (uint32_t i = 0; i < numJobs; ++i) {
            QueueJob(jobs[i]);
        }
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    void JoltEnkiJobSystem::FreeJob(JPH::JobSystem::Job* job) {
        if (job) {
            m_numJobs--;
            delete job;
        }
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    void JoltEnkiJobSystem::WaitForJobs(JPH::JobSystem::Barrier* barrier) {
        // First, call the base class implementation to wait for the barrier
        // This will wait for all Jolt jobs to complete (via semaphore)
        JPH::JobSystemWithBarrier::WaitForJobs(barrier);

        // After the barrier is done, ensure all enkiTS tasks are completed
        // This is necessary because our Jolt jobs are executed by enkiTS tasks
        m_scheduler.waitForAll();
    }

    // NOLINTNEXTLINE(readability-identifier-naming)
    void JoltEnkiJobSystem::JoltJobTask::ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum) {
        // Execute() handles completion/barrier notification; Release()
        // balances QueueJob's AddRef.
        JPH::JobSystem::Job* job = m_job;
        job->Execute();
        job->Release();
    }

}// namespace Vapor
