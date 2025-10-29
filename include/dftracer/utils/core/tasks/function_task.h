#ifndef DFTRACER_UTILS_CORE_TASKS_FUNCTION_TASK_H
#define DFTRACER_UTILS_CORE_TASKS_FUNCTION_TASK_H

#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/tasks/typed_task.h>

#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace dftracer::utils {

/**
 * FunctionTask wraps a std::function with type validation
 * Allows users to provide lambda/function objects instead of subclassing
 */
template <typename I, typename O>
class FunctionTask : public TypedTask<I, O> {
   private:
    std::function<O(I, TaskContext&)> func_;

   public:
    /**
     * Constructor takes a function that accepts input and TaskContext
     */
    explicit FunctionTask(std::function<O(I, TaskContext&)> func)
        : TypedTask<I, O>(), func_(std::move(func)) {
        if (!func_) {
            throw std::invalid_argument("Function cannot be null");
        }
    }

    /**
     * Execute the wrapped function with TaskContext for task emission
     */
    O apply(const I& input) override {
        // This will be set by the executor before calling apply
        TaskContext* context = get_context();
        if (!context) {
            throw std::runtime_error(
                "TaskContext not set - this is an internal error");
        }

        return func_(input, *context);
    }

    void setup_context(TaskContext* context) override { set_context(context); }

    bool needs_context() const override { return true; }

    void set_context(TaskContext* context) { context_ = context; }

    TaskContext* get_context() const { return context_; }

   private:
    TaskContext* context_ = nullptr;
};

template <typename I, typename O>
std::unique_ptr<FunctionTask<I, O>> make_task(
    std::function<O(I, TaskContext&)> func) {
    return std::make_unique<FunctionTask<I, O>>(std::move(func));
}
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_FUNCTION_TASK_H
