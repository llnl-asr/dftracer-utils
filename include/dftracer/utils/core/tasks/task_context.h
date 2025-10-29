#ifndef DFTRACER_UTILS_CORE_TASKS_TASK_CONTEXT_H
#define DFTRACER_UTILS_CORE_TASKS_TASK_CONTEXT_H

#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/pipeline/executors/executor_context.h>
#include <dftracer/utils/core/tasks/task_result.h>
#include <dftracer/utils/core/tasks/task_tag.h>

#include <any>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <typeindex>

namespace dftracer::utils {

class Task;
class TaskContext;
class Scheduler;

template <typename I, typename O>
class FunctionTask;
template <typename I, typename O>
std::unique_ptr<FunctionTask<I, O>> make_task(
    std::function<O(I, TaskContext&)> func);

class TaskContext {
   private:
    Scheduler* scheduler_;
    ExecutorContext* execution_context_;
    TaskIndex current_task_id_;

   public:
    TaskContext(Scheduler* scheduler, ExecutorContext* execution_context,
                TaskIndex current_task_id)
        : scheduler_(scheduler),
          execution_context_(execution_context),
          current_task_id_(current_task_id) {}

    template <typename I, typename O>
    TaskResult<O> emit(std::function<O(I, TaskContext&)> func,
                       const Input<I>& input) {
        auto task = make_task<I, O>(std::move(func));
        TaskIndex task_id =
            execution_context_->add_dynamic_task(std::move(task), -1);

        auto promise = std::make_shared<std::promise<O>>();
        auto future = promise->get_future();

        execution_context_->set_promise_fulfiller(
            task_id, [promise](const std::any& result) {
                try {
                    auto& typed_result = std::any_cast<const O&>(result);
                    promise->set_value(typed_result);
                } catch (const std::future_error& e) {
                    // Promise already set, ignore
                } catch (const std::bad_any_cast& e) {
                    promise->set_exception(
                        std::make_exception_ptr(std::runtime_error(
                            "Type mismatch in dynamic promise fulfillment")));
                }
            });

        execution_context_->set_promise_exception_fulfiller(
            task_id, [promise](std::exception_ptr exception) {
                try {
                    promise->set_exception(exception);
                } catch (const std::future_error& e) {
                    // Promise already set, ignore
                }
            });

        schedule(task_id, input.value());
        return TaskResult<O>{task_id, std::move(future), execution_context_};
    }

    template <typename I, typename O>
    TaskResult<O> emit(std::function<O(I, TaskContext&)> func,
                       DependsOn depends_on) {
        auto task = make_task<I, O>(std::move(func));
        if (depends_on.id >= 0) {
            Task* dep_task = execution_context_->get_task(depends_on.id);
            if (dep_task && task->get_input_type() != typeid(std::any) &&
                dep_task->get_output_type() != task->get_input_type()) {
                throw std::invalid_argument(
                    "Type mismatch: dependency output type " +
                    std::string(dep_task->get_output_type().name()) +
                    " doesn't match task input type " +
                    std::string(task->get_input_type().name()));
            }
        }

        TaskIndex task_id = execution_context_->add_dynamic_task(
            std::move(task), depends_on.id);

        auto promise = std::make_shared<std::promise<O>>();
        auto future = promise->get_future();

        execution_context_->set_promise_fulfiller(
            task_id, [promise](const std::any& result) {
                try {
                    auto& typed_result = std::any_cast<const O&>(result);
                    promise->set_value(typed_result);
                } catch (const std::future_error& e) {
                    // Promise already set, ignore
                } catch (const std::bad_any_cast& e) {
                    promise->set_exception(
                        std::make_exception_ptr(std::runtime_error(
                            "Type mismatch in dynamic promise fulfillment")));
                }
            });

        execution_context_->set_promise_exception_fulfiller(
            task_id, [promise](std::exception_ptr exception) {
                try {
                    promise->set_exception(exception);
                } catch (const std::future_error& e) {
                    // Promise already set, ignore
                }
            });

        return TaskResult<O>{task_id, std::move(future), execution_context_};
    }

    template <typename I, typename O>
    TaskResult<O> emit(std::function<O(I, TaskContext&)> func,
                       const Input<I>& input, DependsOn depends_on) {
        auto task = make_task<I, O>(std::move(func));
        TaskIndex task_id = execution_context_->add_dynamic_task(
            std::move(task), depends_on.id);

        auto promise = std::make_shared<std::promise<O>>();
        auto future = promise->get_future();

        execution_context_->set_promise_fulfiller(
            task_id, [promise](const std::any& result) {
                try {
                    auto& typed_result = std::any_cast<const O&>(result);
                    promise->set_value(typed_result);
                } catch (const std::future_error& e) {
                    // Promise already set, ignore
                } catch (const std::bad_any_cast& e) {
                    promise->set_exception(
                        std::make_exception_ptr(std::runtime_error(
                            "Type mismatch in dynamic promise fulfillment")));
                }
            });

        execution_context_->set_promise_exception_fulfiller(
            task_id, [promise](std::exception_ptr exception) {
                try {
                    promise->set_exception(exception);
                } catch (const std::future_error& e) {
                    // Promise already set, ignore
                }
            });

        schedule(task_id, input.value());
        return TaskResult<O>{task_id, std::move(future), execution_context_};
    }

    TaskIndex current() const { return current_task_id_; }
    void add_dependency(TaskIndex from, TaskIndex to);
    ExecutorContext* get_execution_context() const {
        return execution_context_;
    }

   private:
    void schedule(TaskIndex task_id, const std::any& input);
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_TASK_CONTEXT_H
