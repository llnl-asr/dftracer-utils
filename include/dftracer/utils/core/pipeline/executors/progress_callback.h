#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_PROGRESS_CALLBACK_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_PROGRESS_CALLBACK_H

#include <dftracer/utils/core/common/typedefs.h>

#include <functional>
#include <string>

namespace dftracer::utils {

struct ProgressInfo {
    std::size_t completed_tasks;
    std::size_t total_tasks;
    TaskIndex current_task_id;
    std::string pipeline_name;

    double percentage() const {
        return total_tasks > 0 ? (static_cast<double>(completed_tasks) /
                                  static_cast<double>(total_tasks)) *
                                     100.0
                               : 0.0;
    }
};

using ProgressCallback = std::function<void(const ProgressInfo&)>;

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_PROGRESS_CALLBACK_H
