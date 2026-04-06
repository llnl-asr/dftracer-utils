#ifndef DFTRACER_UTILS_CORE_RUNTIME_H
#define DFTRACER_UTILS_CORE_RUNTIME_H

#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/watchdog.h>
#include <dftracer/utils/core/task_handle.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace dftracer::utils {

/// Lightweight wrapper around Executor + Watchdog for running coroutines
/// on a thread pool without Pipeline/Scheduler/DAG overhead.
/// Intended for Python bindings and other non-DAG consumers.
class Runtime {
   public:
    explicit Runtime(std::size_t threads = 0);
    explicit Runtime(const ExecutorConfig& config, bool enable_watchdog = true);
    Runtime(const ExecutorConfig& config, std::unique_ptr<Watchdog> watchdog);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    /// Async submit returns immediately, task runs on executor.
    TaskHandle submit(coro::CoroTask<void> task, std::string name = "");

    /// Async submit with typed result.
    template <typename T>
    TypedTaskHandle<T> submit(coro::CoroTask<T> task, std::string name = "");

    /// Wait for all outstanding tasks to complete.
    void wait_all();

    ExecutorProgress get_progress() const;
    bool is_responsive() const;

    void set_global_timeout(std::chrono::milliseconds timeout);
    void set_default_task_timeout(std::chrono::milliseconds timeout);

    void shutdown();
    std::size_t threads() const;
    Executor* executor() { return executor_.get(); }
    Watchdog* watchdog() { return watchdog_.get(); }

   private:
    void cleanup_completed_futures();

    std::unique_ptr<Executor> executor_;
    std::unique_ptr<Watchdog> watchdog_;
    std::size_t threads_;
    std::atomic<bool> shutdown_called_{false};
    std::atomic<uint64_t> task_name_counter_{0};
    std::vector<std::shared_future<void>> outstanding_futures_;
    std::mutex futures_mutex_;
};

template <typename T>
TypedTaskHandle<T> Runtime::submit(coro::CoroTask<T> task, std::string name) {
    if (shutdown_called_.load(std::memory_order_acquire)) {
        throw std::runtime_error("Runtime is shut down");
    }
    if (name.empty()) {
        name = "task-" + std::to_string(task_name_counter_++);
    }

    auto typed_promise = std::make_shared<std::promise<T>>();
    auto typed_future = typed_promise->get_future().share();

    // void future for outstanding_futures_ tracking
    auto void_promise = std::make_shared<std::promise<void>>();
    auto void_future = void_promise->get_future().share();

    auto tid = std::make_shared<std::atomic<TaskIndex>>(-1);

    auto wrapper =
        [](coro::CoroTask<T> t, std::shared_ptr<std::promise<T>> tp,
           std::shared_ptr<std::promise<void>> vp, Executor* exec,
           std::shared_ptr<std::atomic<TaskIndex>> task_id) -> coro::Coro {
        try {
            T val = co_await std::move(t);
            t = coro::CoroTask<T>{std::coroutine_handle<
                typename coro::CoroTask<T>::promise_type>{}};
            exec->mark_coro_completed(task_id->load(std::memory_order_acquire));
            tp->set_value(std::move(val));
        } catch (...) {
            t = coro::CoroTask<T>{std::coroutine_handle<
                typename coro::CoroTask<T>::promise_type>{}};
            exec->mark_coro_completed(task_id->load(std::memory_order_acquire));
            auto ex = std::current_exception();
            tp->set_exception(ex);
            vp->set_exception(ex);
            co_return;
        }
        vp->set_value();
    };

    auto coro = wrapper(std::move(task), typed_promise, void_promise,
                        executor_.get(), tid);
    TaskIndex id = executor_->enqueue_tracked(std::move(coro), name, tid);

    {
        std::lock_guard<std::mutex> lock(futures_mutex_);
        cleanup_completed_futures();
        outstanding_futures_.push_back(void_future);
    }

    return TypedTaskHandle<T>{typed_future, id, std::move(name)};
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_RUNTIME_H
