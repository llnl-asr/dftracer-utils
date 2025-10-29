#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_THREAD_EXECUTOR_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_THREAD_EXECUTOR_H

#include <dftracer/utils/core/pipeline/executors/executor.h>
#include <dftracer/utils/core/pipeline/executors/scheduler/thread_scheduler.h>

#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <vector>

namespace dftracer::utils {

class ThreadExecutor : public Executor {
   private:
    std::size_t max_threads_;
    ThreadScheduler scheduler_;

   public:
    ThreadExecutor();
    explicit ThreadExecutor(size_t max_threads);
    ~ThreadExecutor() override;
    ThreadExecutor(const ThreadExecutor&) = delete;
    ThreadExecutor& operator=(const ThreadExecutor&) = delete;
    ThreadExecutor(ThreadExecutor&&) = default;
    ThreadExecutor& operator=(ThreadExecutor&&) = default;

    PipelineOutput execute(const Pipeline& pipeline, std::any input) override;
    virtual void reset() override;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_THREAD_EXECUTOR_H
