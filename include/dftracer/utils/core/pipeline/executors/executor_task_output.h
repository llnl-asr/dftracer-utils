#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_TASK_OUTPUT_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_TASK_OUTPUT_H

#include <any>
#include <atomic>

namespace dftracer::utils {

struct ExecutorTaskOutput {
    std::any data;
    std::atomic<int> dependency_refs{0};
    std::atomic<int> user_refs{0};

    bool can_cleanup() const {
        return dependency_refs.load() == 0 && user_refs.load() == 0;
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_TASK_OUTPUT_H