#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/pipeline/executors/executor_context.h>
#include <dftracer/utils/core/pipeline/executors/scheduler/thread_scheduler.h>
#include <dftracer/utils/core/pipeline/executors/thread_executor.h>

#include <any>
#include <thread>

namespace dftracer::utils {

ThreadExecutor::ThreadExecutor()
    : Executor(ExecutorType::THREAD),
      max_threads_(std::thread::hardware_concurrency()) {
    if (max_threads_ == 0) max_threads_ = 2;
    scheduler_.initialize(max_threads_);
    DFTRACER_UTILS_LOG_DEBUG(
        "ThreadExecutor initialized with max_threads = %zu", max_threads_);
}

ThreadExecutor::ThreadExecutor(size_t max_threads)
    : Executor(ExecutorType::THREAD), max_threads_(max_threads) {
    if (max_threads_ == 0) {
        max_threads_ = 2;
    }
    scheduler_.initialize(max_threads_);
    DFTRACER_UTILS_LOG_DEBUG(
        "ThreadExecutor initialized with max_threads = %zu", max_threads_);
}

ThreadExecutor::~ThreadExecutor() { scheduler_.shutdown(); }

void ThreadExecutor::reset() { scheduler_.reset(); }

PipelineOutput ThreadExecutor::execute(const Pipeline& pipeline,
                                       std::any input) {
    scheduler_.set_progress_callback(progress_callback_);
    PipelineOutput result = scheduler_.execute(pipeline, input);
    return result;
}

}  // namespace dftracer::utils
