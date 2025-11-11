#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/tasks/task_scope.h>

#include <type_traits>

namespace dftracer::utils {

struct JoinAllAwaitable {
    std::vector<TaskFuture<std::any>>& futures;
    TaskContext* context;
    std::shared_ptr<std::atomic<size_t>> completed_count;
    TaskIndex join_id;

    JoinAllAwaitable(std::vector<TaskFuture<std::any>>& futs, TaskContext* ctx)
        : futures(futs),
          context(ctx),
          completed_count(std::make_shared<std::atomic<size_t>>(0)),
          join_id(generate_unique_join_id()) {}

    static TaskIndex generate_unique_join_id() {
        static std::atomic<TaskIndex> counter{-1000000};
        return counter.fetch_sub(1, std::memory_order_relaxed);
    }

    bool await_ready() {
        return std::all_of(futures.begin(), futures.end(),
                           [](auto& f) { return f.await_ready(); });
    }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) {
        size_t total = futures.size();

        if (total == 0) {
            return h;
        }

        auto* executor = context->get_executor();
        if (!executor) {
            return h;
        }

        auto* scheduler = context->get_scheduler();
        if (!scheduler) {
            return h;
        }

        for (auto& future : futures) {
            if (future.await_ready()) {
                size_t old =
                    completed_count->fetch_add(1, std::memory_order_acq_rel);
                if (old + 1 == total) {
                    return h;
                }
                continue;
            }

            TaskIndex task_id = future.get_task_id();
            auto count_ptr = completed_count;

            scheduler->register_task_completion_callback(
                task_id, [h, executor, count_ptr, total]() {
                    size_t old =
                        count_ptr->fetch_add(1, std::memory_order_acq_rel);
                    if (old + 1 == total) {
                        executor->schedule_coroutine_resumption(h);
                    }
                });
        }

        if constexpr (std::is_base_of_v<coro::PromiseBase, Promise>) {
            auto* root = h.promise().get_root_promise();
            root->set_awaited_task_id(join_id);
            root->awaiting_async_ = true;
        }

        return std::noop_coroutine();
    }

    void await_resume() {}
};

TaskFuture<std::any> TaskContext::spawn_untracked(std::shared_ptr<Task> task,
                                                  const std::any& input) {
    if (!scheduler_) {
        throw std::runtime_error(
            "TaskContext: No scheduler available for task spawning");
    }

    if (!task) {
        throw std::invalid_argument("TaskContext: Cannot spawn null task");
    }

    DFTRACER_UTILS_LOG_DEBUG("Spawning task '%s' from parent task ID %d",
                             task->get_name().c_str(), current_task_id_);

    scheduler_->submit_dynamic_task(task, input);

    return TaskFuture<std::any>(task, task->get_id(), scheduler_,
                                cancellation_requested_);
}

coro::CoroTask<void> TaskContext::join_all() {
    if (spawned_tasks_.empty()) {
        co_return;
    }

    co_await JoinAllAwaitable(spawned_tasks_, this);
    spawned_tasks_.clear();

    co_return;
}

bool TaskContext::has_async_io() const {
    if (!executor_) {
        return false;
    }

    return executor_->has_io_executor();
}

}  // namespace dftracer::utils
