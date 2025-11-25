#include "jolt_enki_job_system.hpp"
#include <thread>
#include <fmt/core.h>

namespace Vapor {

JoltEnkiJobSystem::JoltEnkiJobSystem(TaskScheduler& scheduler, uint32_t maxJobs)
    : JPH::JobSystem(maxJobs)
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

    // Allocate a new job
    Job* job = AllocateJob();
    job->m_JobFunction = jobFunction;
    job->SetBarrier(nullptr);

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
        JPH::JobSystem::FreeJob(job);
    }
}

void JoltEnkiJobSystem::JoltJobTask::ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum) {
    // Execute the Jolt job function
    m_job->Execute();

    // Release the job
    m_job->Release();
}

} // namespace Vapor
