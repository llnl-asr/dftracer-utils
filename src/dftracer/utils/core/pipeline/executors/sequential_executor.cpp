#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/pipeline/executors/scheduler/sequential_scheduler.h>
#include <dftracer/utils/core/pipeline/executors/sequential_executor.h>
#include <dftracer/utils/core/tasks/task_context.h>

#include <any>
#include <unordered_map>

namespace dftracer::utils {

SequentialExecutor::SequentialExecutor() : Executor(ExecutorType::SEQUENTIAL) {}

PipelineOutput SequentialExecutor::execute(const Pipeline& pipeline,
                                           std::any input) {
    SequentialScheduler scheduler;
    scheduler.set_progress_callback(progress_callback_);
    return scheduler.execute(pipeline, input);
}

}  // namespace dftracer::utils
