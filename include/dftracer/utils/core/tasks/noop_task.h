#ifndef DFTRACER_UTILS_CORE_TASKS_NOOP_TASK_H
#define DFTRACER_UTILS_CORE_TASKS_NOOP_TASK_H

#include <dftracer/utils/core/tasks/task.h>

#include <memory>

namespace dftracer::utils {

/**
 * NoOpTask - Pass-through task for multiple source nodes
 *
 * This task is automatically created by Pipeline when multiple source tasks
 * are specified. It simply passes through its input unchanged.
 *
 * Purpose:
 * - Ensures every pipeline has exactly one source node
 * - Provides trackability for multiple independent starting points
 */
class NoOpTask : public Task {
   public:
    NoOpTask()
        : Task(
              []() {
                  // No-op: just a synchronization point for multiple sources
              },
              "__noop_source__") {}

    virtual ~NoOpTask() = default;
};

/**
 * Helper function to create a NoOpTask
 */
inline std::shared_ptr<NoOpTask> make_noop_task() {
    return std::make_shared<NoOpTask>();
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_NOOP_TASK_H
