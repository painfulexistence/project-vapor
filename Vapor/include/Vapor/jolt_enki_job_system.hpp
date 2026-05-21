#ifndef JOLT_ENKI_JOB_SYSTEM_HPP
#define JOLT_ENKI_JOB_SYSTEM_HPP

#include "task_scheduler.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemWithBarrier.h>
#include <atomic>

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

        // enkiTS task wrapper for Jolt jobs
        class JoltJobTask : public enki::ITaskSet {
        public:
            JoltJobTask(JPH::JobSystem::Job* job) : m_job(job) {
            }

            // NOLINTNEXTLINE(readability-identifier-naming)
            void ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum) override;

        private:
            JPH::JobSystem::Job* m_job;
        };
    };

}// namespace Vapor

#endif// JOLT_ENKI_JOB_SYSTEM_HPP
