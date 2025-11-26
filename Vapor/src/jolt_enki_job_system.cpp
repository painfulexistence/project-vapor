#include "jolt_enki_job_system.hpp"
#include <thread>
#include <fmt/core.h>

namespace Vapor {

JoltEnkiJobSystem::JoltEnkiJobSystem(TaskScheduler& scheduler, uint32_t maxJobs)
    : JPH::JobSystemWithBarrier(256)  // Initialize with max barriers
    , m_scheduler(scheduler)
    , m_maxJobs(maxJobs) {

    fmt::print("JoltEnkiJobSystem: Initialized with max {} jobs\n", maxJobs);
}

JoltEnkiJobSystem::~JoltEnkiJobSystem() {
    // Wait for all jobs to complete
    m_scheduler.waitForAll();
}

int JoltEnkiJobSystem::GetMaxConcurrency() const {
    // Return hardware thread count
    int concurrency = std::thread::hardware_concurrency();
    return concurrency > 0 ? concurrency : 4;
}

JPH::JobHandle JoltEnkiJobSystem::CreateJob(const char* name, JPH::ColorArg color,
                                             const JPH::JobSystem::JobFunction& jobFunction,
                                             uint32_t numDependencies) {
    // Check if we have available job slots
    while (m_numJobs.load() >= m_maxJobs) {
        std::this_thread::yield();
    }

    // Create a new job using the protected Job constructor
    Job* job = new Job(name, color, this, jobFunction, numDependencies);

    // If no dependencies, queue it immediately
    if (numDependencies == 0) {
        QueueJob(job);
    }

    m_numJobs++;

    return JPH::JobHandle(job);
}

void JoltEnkiJobSystem::QueueJob(JPH::JobSystem::Job* job) {
    if (!job) return;

    // Create enkiTS task wrapper
    auto task = new JoltJobTask(job);

    // Submit to enkiTS scheduler
    m_scheduler.getScheduler()->AddTaskSetToPipe(task);
}

void JoltEnkiJobSystem::QueueJobs(JPH::JobSystem::Job** jobs, uint32_t numJobs) {
    for (uint32_t i = 0; i < numJobs; ++i) {
        QueueJob(jobs[i]);
    }
}

void JoltEnkiJobSystem::FreeJob(JPH::JobSystem::Job* job) {
    if (job) {
        m_numJobs--;
        delete job;
    }
}

void JoltEnkiJobSystem::WaitForJobs(JPH::JobSystem::Barrier* barrier) {
    // First, call the base class implementation to wait for the barrier
    // This will wait for all Jolt jobs to complete (via semaphore)
    JPH::JobSystemWithBarrier::WaitForJobs(barrier);

    // After the barrier is done, ensure all enkiTS tasks are completed
    // This is necessary because our Jolt jobs are executed by enkiTS tasks
    m_scheduler.waitForAll();
}

void JoltEnkiJobSystem::JoltJobTask::ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum) {
    // Execute the Jolt job function
    // Execute() will handle job completion, barrier notification, etc.
    // We should NOT call Release() here - JobHandle manages the reference count.
    // When all JobHandles are destroyed, the job will be automatically freed.
    m_job->Execute();
}

} // namespace Vapor
