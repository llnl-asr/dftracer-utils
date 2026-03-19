#ifndef DFTRACER_UTILS_CORE_RUNTIME_H
#define DFTRACER_UTILS_CORE_RUNTIME_H

#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/watchdog.h>

#include <any>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

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

    /// Block until task completes, return its result (or rethrow).
    template <typename T>
    T submit(std::string name, coro::CoroTask<T> task);

    /// Block until void task completes (or rethrow).
    void submit(std::string name, coro::CoroTask<void> task);

    /// Fire-and-forget: enqueue task without waiting.
    void schedule(std::string name, coro::CoroTask<void> task);

    ExecutorProgress get_progress() const;
    bool is_responsive() const;

    void set_global_timeout(std::chrono::milliseconds timeout);
    void set_default_task_timeout(std::chrono::milliseconds timeout);

    void shutdown();
    std::size_t threads() const;
    Executor* executor() { return executor_.get(); }
    Watchdog* watchdog() { return watchdog_.get(); }

    struct SubmitResult {
        std::exception_ptr exception;
        std::any value;
        std::promise<void> signal;
    };

   private:
    coro::Coro make_submit_coro(coro::CoroTask<void> task,
                                std::shared_ptr<SubmitResult> result,
                                Executor* exec,
                                std::shared_ptr<std::atomic<TaskIndex>> tid);

    template <typename T>
    coro::Coro make_submit_coro(coro::CoroTask<T> task,
                                std::shared_ptr<SubmitResult> result,
                                Executor* exec,
                                std::shared_ptr<std::atomic<TaskIndex>> tid);

    coro::Coro make_schedule_coro(coro::CoroTask<void> task, Executor* exec,
                                  std::shared_ptr<std::atomic<TaskIndex>> tid);

    std::unique_ptr<Executor> executor_;
    std::unique_ptr<Watchdog> watchdog_;
    std::size_t threads_;
    std::atomic<bool> shutdown_called_{false};
};

template <typename T>
T Runtime::submit(std::string name, coro::CoroTask<T> task) {
    if (shutdown_called_.load(std::memory_order_acquire)) {
        throw std::runtime_error("Runtime is shut down");
    }
    auto tid = std::make_shared<std::atomic<TaskIndex>>(-1);
    auto result = std::make_shared<SubmitResult>();
    auto future = result->signal.get_future();
    auto coro = make_submit_coro(std::move(task), result, executor_.get(), tid);
    executor_->enqueue_tracked(std::move(coro), std::move(name), tid);
    future.get();
    if (result->exception) std::rethrow_exception(result->exception);
    return std::any_cast<T>(std::move(result->value));
}

template <typename T>
coro::Coro Runtime::make_submit_coro(
    coro::CoroTask<T> task, std::shared_ptr<SubmitResult> result,
    Executor* exec, std::shared_ptr<std::atomic<TaskIndex>> tid) {
    try {
        auto val = co_await std::move(task);
        exec->mark_coro_completed(tid->load(std::memory_order_acquire));
        result->value = std::move(val);
    } catch (...) {
        exec->mark_coro_completed(tid->load(std::memory_order_acquire));
        result->exception = std::current_exception();
    }
    result->signal.set_value();
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_RUNTIME_H
