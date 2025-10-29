#ifndef DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_H
#define DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_H

#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/pipeline/executors/executor_context.h>
#include <dftracer/utils/core/tasks/tasks.h>

#include <any>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <queue>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dftracer::utils {
class Pipeline {
   protected:
    std::vector<std::unique_ptr<Task>> nodes_;
    std::vector<std::vector<TaskIndex>> dependencies_;
    std::vector<std::vector<TaskIndex>> dependents_;

    std::unordered_map<TaskIndex, std::function<void(const std::any&)>>
        promise_fulfillers_;
    std::unordered_map<TaskIndex, std::function<void(std::exception_ptr)>>
        promise_exception_fulfillers_;

    std::string name_;

   public:
    Pipeline() = default;
    explicit Pipeline(std::string name) : name_(std::move(name)) {}
    virtual ~Pipeline() = default;

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = default;
    Pipeline& operator=(Pipeline&&) = default;

    void add_dependency(TaskIndex from, TaskIndex to);

    template <typename I, typename O>
    TaskResult<O> add_task(std::function<O(I, TaskContext&)> func,
                           TaskIndex depends_on = -1) {
        auto task = make_task<I, O>(std::move(func));
        TaskIndex task_id = add_task(std::move(task), depends_on);

        auto promise = std::make_shared<std::promise<O>>();
        auto future = promise->get_future();

        promise_fulfillers_[task_id] = [promise](const std::any& result) {
            try {
                auto& typed_result = std::any_cast<const O&>(result);
                promise->set_value(typed_result);
            } catch (const std::future_error& e) {
            } catch (const std::bad_any_cast& e) {
                promise->set_exception(
                    std::make_exception_ptr(std::runtime_error(
                        "Type mismatch in promise fulfillment: " +
                        std::string(e.what()))));
            }
        };

        promise_exception_fulfillers_[task_id] =
            [promise](std::exception_ptr exception) {
                try {
                    promise->set_exception(exception);
                } catch (const std::future_error& e) {
                }
            };

        return TaskResult<O>{task_id, std::move(future)};
    }

    void chain(Pipeline&& other);

    std::size_t size() const { return nodes_.size(); }
    bool empty() const { return nodes_.empty(); }

    void set_name(std::string name) { name_ = std::move(name); }
    const std::string& get_name() const { return name_; }

    inline const std::vector<std::unique_ptr<Task>>& get_nodes() const {
        return nodes_;
    }
    inline const std::vector<std::vector<TaskIndex>>& get_dependencies() const {
        return dependencies_;
    }
    inline const std::vector<std::vector<TaskIndex>>& get_dependents() const {
        return dependents_;
    }
    inline Task* get_task(TaskIndex index) const {
        if (index < 0) return nullptr;
        return index < static_cast<TaskIndex>(nodes_.size())
                   ? nodes_[index].get()
                   : nullptr;
    }
    inline const std::vector<TaskIndex>& get_task_dependencies(
        TaskIndex index) const {
        return dependencies_[index];
    }
    inline const std::vector<TaskIndex>& get_task_dependents(
        TaskIndex index) const {
        return dependents_[index];
    }

    bool validate_types() const;
    bool has_cycles() const;
    std::vector<TaskIndex> topological_sort() const;

    void fulfill_promise(TaskIndex task_id, const std::any& result) const {
        auto it = promise_fulfillers_.find(task_id);
        if (it != promise_fulfillers_.end()) {
            it->second(result);
        }
    }

    void fulfill_promise_exception(TaskIndex task_id,
                                   std::exception_ptr exception) const {
        auto it = promise_exception_fulfillers_.find(task_id);
        if (it != promise_exception_fulfillers_.end()) {
            it->second(exception);
        }
    }

    template <typename O>
    void register_dynamic_promise(TaskIndex task_id,
                                  std::shared_ptr<std::promise<O>> promise) {
        promise_fulfillers_[task_id] = [promise](const std::any& result) {
            try {
                auto& typed_result = std::any_cast<const O&>(result);
                promise->set_value(typed_result);
            } catch (const std::future_error& e) {
            } catch (const std::bad_any_cast& e) {
                promise->set_exception(
                    std::make_exception_ptr(std::runtime_error(
                        "Type mismatch in dynamic promise fulfillment: " +
                        std::string(e.what()))));
            }
        };

        promise_exception_fulfillers_[task_id] =
            [promise](std::exception_ptr exception) {
                try {
                    promise->set_exception(exception);
                } catch (const std::future_error& e) {
                }
            };
    }

   protected:
    TaskIndex add_task(std::unique_ptr<Task> task, TaskIndex depends_on = -1);
};
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_H
