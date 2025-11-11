#ifndef DFTRACER_UTILS_CORE_PIPELINE_TASK_ITEM_H
#define DFTRACER_UTILS_CORE_PIPELINE_TASK_ITEM_H

#include <any>
#include <memory>

namespace dftracer::utils {

class Task;

struct TaskItem {
    std::shared_ptr<Task> task;
    std::shared_ptr<std::any> input;

    TaskItem() = default;
    TaskItem(std::shared_ptr<Task> t, std::shared_ptr<std::any> i)
        : task(std::move(t)), input(std::move(i)) {}
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_TASK_ITEM_H
