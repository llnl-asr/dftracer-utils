#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_TASK_ITEM_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_TASK_ITEM_H

#include <dftracer/utils/core/common/typedefs.h>

#include <any>
#include <memory>
#include <variant>
#include <vector>

namespace dftracer::utils {

class Task;

struct TaskItem {
    TaskIndex task_id;
    Task* task_ptr;
    std::shared_ptr<std::any> input;

    TaskItem() : task_id(-1), task_ptr(nullptr) {}

    TaskItem(TaskIndex id, Task* ptr, std::shared_ptr<std::any> inp)
        : task_id(id), task_ptr(ptr), input(inp) {}

    TaskItem(TaskIndex id, Task* ptr, std::any&& inp)
        : task_id(id),
          task_ptr(ptr),
          input(std::make_shared<std::any>(std::move(inp))) {}

    const std::any& get_input() const { return *input; }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_SCHEDULER_TASK_ITEM_H
