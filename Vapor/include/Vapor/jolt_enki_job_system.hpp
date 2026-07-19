#pragma once

#include "task_scheduler.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemWithBarrier.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace Vapor {

    /**
     * Jolt Physics job system implementation using enkiTS
     * This allows Jolt to use the same task scheduler as resource loading and rendering
     */
    class JoltEnkiJobSystem : public JPH::JobSystemWithBarrier {
    public:
        explicit JoltEnkiJobSystem(TaskScheduler& scheduler, uint32_t maxJobs = 2048);
        virtual ~JoltEnkiJobSystem();

        // JobSystem interface implementation
        // NOLINTNEXTLINE(readability-identifier-naming)
        virtual int GetMaxConcurrency() const override;
        // NOLINTNEXTLINE(readability-identifier-naming)
        virtual JPH::JobHandle CreateJob(
            const char* name,
            JPH::ColorArg color,
            const JPH::JobSystem::JobFunction& jobFunction,
            uint32_t numDependencies = 0
        ) override;
        // NOLINTNEXTLINE(readability-identifier-naming)
        virtual void QueueJob(JPH::JobSystem::Job* job) override;
        // NOLINTNEXTLINE(readability-identifier-naming)
        virtual void QueueJobs(JPH::JobSystem::Job** jobs, uint32_t numJobs) override;
        // NOLINTNEXTLINE(readability-identifier-naming)
        virtual void FreeJob(JPH::JobSystem::Job* job) override;
        // NOLINTNEXTLINE(readability-identifier-naming)
        virtual void WaitForJobs(JPH::JobSystem::Barrier* barrier) override;

    private:
        TaskScheduler& m_scheduler;
        std::atomic<uint32_t> m_numJobs{ 0 };
        uint32_t m_maxJobs;

        // enkiTS task wrapper for Jolt jobs. Instances are pooled (see
        // QueueJob): enkiTS never takes ownership of tasks handed to
        // AddTaskSetToPipe, so heap-allocating one per job leaked one per job.
        class JoltJobTask : public enki::ITaskSet {
        public:
            JoltJobTask() = default;

            void setJob(JPH::JobSystem::Job* job) { m_job = job; }

            // NOLINTNEXTLINE(readability-identifier-naming)
            void ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum) override;

        private:
            JPH::JobSystem::Job* m_job = nullptr;
        };

        // Task pool, reused via GetIsComplete() (enkiTS's documented
        // safe-to-destroy signal — reuse is destroy-equivalent). Guarded by
        // m_taskPoolMutex across BOTH the pick and AddTaskSetToPipe, so two
        // threads can't grab the same still-complete task. Bounded by the
        // maximum number of simultaneously in-flight jobs.
        std::mutex m_taskPoolMutex;
        std::vector<std::unique_ptr<JoltJobTask>> m_taskPool;
    };

}// namespace Vapor
