#include "jolt_enki_job_system.hpp"
#include <fmt/core.h>

namespace Vapor {

JoltEnkiJobSystem::JoltEnkiJobSystem(TaskScheduler& scheduler, uint32_t maxJobs)
    : JPH::JobSystemWithBarrier(maxJobs) // Jolt's internal barrier system
    , m_scheduler(scheduler)
    , m_jobPool(maxJobs)
    , m_taskPool(maxJobs) {
    fmt::print("JoltEnkiJobSystem: Initialized with max {} jobs and {} threads.\n", maxJobs, GetMaxConcurrency());
}

int JoltEnkiJobSystem::GetMaxConcurrency() const {
    return m_scheduler.getNumThreads();
}

JPH::JobHandle JoltEnkiJobSystem::CreateJob(const char* name, JPH::ColorArg color,
                                             const JPH::JobSystem::JobFunction& jobFunction,
                                             uint32_t numDependencies) {
    // Acquire a job from the pool. This will block if the pool is empty.
    VaporJob* job = m_jobPool.acquire();
    job->Set(name, color, this, jobFunction, numDependencies);
    return JPH::JobHandle(job);
}

void JoltEnkiJobSystem::QueueJob(JPH::JobSystem::Job* job) {
    if (!job) return;

    // Acquire a task from the pool
    JoltJobTask* task = m_taskPool.acquire();
    task->setJob(job, this);

    // Submit to enkiTS scheduler
    m_scheduler.getScheduler()->AddTaskSetToPipe(task);
}

void JoltEnkiJobSystem::QueueJobs(JPH::JobSystem::Job** jobs, uint32_t numJobs) {
    if (numJobs == 0) return;

    // Acquire tasks for all jobs
    std::vector<JoltJobTask*> tasks;
    tasks.reserve(numJobs);
    for (uint32_t i = 0; i < numJobs; ++i) {
        JoltJobTask* task = m_taskPool.acquire();
        task->setJob(jobs[i], this);
        tasks.push_back(task);
    }

    // Submit all tasks at once
    m_scheduler.getScheduler()->AddTaskSetToPipe(tasks.data(), numJobs);
}


void JoltEnkiJobSystem::FreeJob(JPH::JobSystem::Job* job) {
    if (job) {
        // We must cast the Job back to the derived VaporJob to release it to the correct pool.
        m_jobPool.release(static_cast<VaporJob*>(job));
    }
}

void JoltEnkiJobSystem::JoltJobTask::ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum) {
    // Execute the Jolt job function. This will handle job completion, barrier notification, etc.
    m_job->Execute();

    // After execution, release this task back to the pool for reuse.
    m_system->m_taskPool.release(this);
}

} // namespace Vapor

