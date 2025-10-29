#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SEQUENTIAL_EXECUTOR_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SEQUENTIAL_EXECUTOR_H

#include <dftracer/utils/core/pipeline/executors/executor.h>

namespace dftracer::utils {

class SequentialExecutor : public Executor {
   public:
    SequentialExecutor();
    ~SequentialExecutor() override = default;
    SequentialExecutor(const SequentialExecutor&) = delete;
    SequentialExecutor& operator=(const SequentialExecutor&) = delete;
    SequentialExecutor(SequentialExecutor&&) = default;
    SequentialExecutor& operator=(SequentialExecutor&&) = default;

    PipelineOutput execute(const Pipeline& pipeline, std::any input) override;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SEQUENTIAL_EXECUTOR_H
