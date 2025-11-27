#ifndef JOLT_ENKI_JOB_SYSTEM_HPP
#define JOLT_ENKI_JOB_SYSTEM_HPP

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include "task_scheduler.hpp"
#include "object_pool.hpp"

namespace Vapor {

/**
 * Jolt Physics job system implementation using enkiTS
 * This allows Jolt to use the same task scheduler as resource loading and rendering
 *
 * This implementation uses object pools to avoid heap allocations during runtime.
 */
class JoltEnkiJobSystem : public JPH::JobSystemWithBarrier {
public:
    explicit JoltEnkiJobSystem(TaskScheduler& scheduler, uint32_t maxJobs = 2048);

    // JobSystem interface implementation
    virtual int GetMaxConcurrency() const override;
    virtual JPH::JobHandle CreateJob(const char* name, JPH::ColorArg color,
                                     const JPH::JobSystem::JobFunction& jobFunction,
                                     uint32_t numDependencies = 0) override;
    virtual void QueueJob(JPH::JobSystem::Job* job) override;
    virtual void QueueJobs(JPH::JobSystem::Job** jobs, uint32_t numJobs) override;
    virtual void FreeJob(JPH::JobSystem::Job* job) override;

    // The Jolt documentation states that this function should not be called when using barriers.
    // The barrier implementation in JobSystemWithBarrier handles waiting.
    // See: https://jrouwe.github.io/JoltPhysics/class_j_p_h_1_1_job_system.html#a59253509b55f17478631b14b9b222d51
    virtual void WaitForJobs(JPH::JobSystem::Barrier* barrier) override {
      JobSystemWithBarrier::WaitForJobs(barrier);
    }

private:
    friend class JoltJobTask;

    // Helper struct to publicly expose the Job constructor
    struct VaporJob : public JPH::Job {
        VaporJob() : JPH::Job("", JPH::Color::sWhite, nullptr, nullptr, 0) {}
        void Set(const char* name, JPH::ColorArg color, JPH::JobSystem* system,
                 const JPH::JobSystem::JobFunction& function, uint32_t numDependencies) {
            mName = name;
            mColor = color;
            mJobSystem = system;
            mJobFunction = function;
            mNumDependencies = numDependencies;
            mIsDone = false;
        }
    };

    // enkiTS task wrapper for Jolt jobs
    class JoltJobTask : public enki::ITaskSet {
    public:
        JoltJobTask() = default;

        void setJob(JPH::JobSystem::Job* job, JoltEnkiJobSystem* system) {
            m_job = job;
            m_system = system;
        }

        void ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum) override;

    private:
        JPH::JobSystem::Job* m_job = nullptr;
        JoltEnkiJobSystem* m_system = nullptr;
    };

    TaskScheduler& m_scheduler;
    ObjectPool<VaporJob> m_jobPool;
    ObjectPool<JoltJobTask> m_taskPool;
};

} // namespace Vapor

#endif // JOLT_ENKI_JOB_SYSTEM_HPP
