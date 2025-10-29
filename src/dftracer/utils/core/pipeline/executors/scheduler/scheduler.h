#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_SCHEDULER_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_SCHEDULER_H

#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/pipeline/executors/progress_callback.h>
#include <dftracer/utils/core/pipeline/pipeline_output.h>

#include <any>

namespace dftracer::utils {
class Pipeline;

class Scheduler {
   public:
    virtual ~Scheduler() = default;

    virtual void reset() {
        completed_tasks_ = 0;
        total_tasks_ = 0;
    }

    virtual PipelineOutput execute(const Pipeline& pipeline,
                                   const std::any& input) = 0;

    virtual void set_progress_callback(ProgressCallback callback) {
        progress_callback_ = std::move(callback);
    }

   protected:
    void report_progress(TaskIndex task_id,
                         const std::string& pipeline_name = "") {
        if (progress_callback_) {
            ++completed_tasks_;
            ProgressInfo info{completed_tasks_, total_tasks_, task_id,
                              pipeline_name};
            progress_callback_(info);
        }
    }

    ProgressCallback progress_callback_;
    std::size_t completed_tasks_{0};
    std::size_t total_tasks_{0};
    std::string current_pipeline_name_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_SCHEDULER_H
