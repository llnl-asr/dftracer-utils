#ifndef DFTRACER_UTILS_CORE_TASKS_NOOP_TASK_H
#define DFTRACER_UTILS_CORE_TASKS_NOOP_TASK_H

#include <dftracer/utils/core/tasks/task.h>

#include <memory>

namespace dftracer::utils {

/**
 * NoOpTask - Pass-through task for multiple source or destination nodes
 *
 * This task is automatically created by Pipeline when multiple source or
 * destination tasks are specified. It simply acts as a synchronization point.
 *
 * Purpose:
 * - Ensures every pipeline has exactly one source/destination node
 * - Provides trackability for multiple independent starting/ending points
 * - Returns void to bypass type validation (works with any parent types)
 */
class NoOpTask : public Task {
   public:
    explicit NoOpTask(std::string name = "__noop__")
        : Task(
              []() -> void {
                  // No-op: just a synchronization point
              },
              std::move(name)) {}

    virtual ~NoOpTask() = default;
};

/**
 * Helper function to create a NoOpTask
 */
inline std::shared_ptr<NoOpTask> make_noop_task(std::string name = "__noop__") {
    return std::make_shared<NoOpTask>(std::move(name));
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_NOOP_TASK_H
