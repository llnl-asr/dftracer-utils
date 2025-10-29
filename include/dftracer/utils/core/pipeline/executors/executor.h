#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_H

#include <dftracer/utils/core/pipeline/executors/executor_type.h>
#include <dftracer/utils/core/pipeline/executors/progress_callback.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_output.h>

#include <any>
#include <string>

namespace dftracer::utils {
class Executor {
   protected:
    Executor(ExecutorType type) : type_(type) {}

   public:
    virtual ~Executor() = default;
    virtual PipelineOutput execute(const Pipeline& pipeline,
                                   std::any input) = 0;
    inline ExecutorType type() const { return type_; }
    virtual void reset() {}

    virtual void set_progress_callback(ProgressCallback callback) {
        progress_callback_ = std::move(callback);
    }

   public:
    const ExecutorType type_;

   protected:
    ProgressCallback progress_callback_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_H
