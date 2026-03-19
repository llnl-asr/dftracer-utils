#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/runtime.h>

#include <stdexcept>
#include <thread>

namespace dftracer::utils {

Runtime::Runtime(std::size_t threads)
    : threads_(threads == 0 ? std::thread::hardware_concurrency() : threads) {
    ExecutorConfig config;
    config.num_threads = threads_;
    executor_ = std::make_unique<Executor>(config);
    executor_->start();

    watchdog_ = std::make_unique<Watchdog>();
    watchdog_->set_executor(executor_.get());
    watchdog_->start();
}

Runtime::Runtime(const ExecutorConfig& config, bool enable_watchdog)
    : threads_(config.num_threads == 0 ? std::thread::hardware_concurrency()
                                       : config.num_threads) {
    executor_ = std::make_unique<Executor>(config);
    executor_->start();

    if (enable_watchdog) {
        watchdog_ = std::make_unique<Watchdog>();
        watchdog_->set_executor(executor_.get());
        watchdog_->start();
    }
}

Runtime::~Runtime() { shutdown(); }

void Runtime::submit(std::string name, coro::CoroTask<void> task) {
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
}

void Runtime::schedule(std::string name, coro::CoroTask<void> task) {
    if (shutdown_called_.load(std::memory_order_acquire)) {
        throw std::runtime_error("Runtime is shut down");
    }
    auto tid = std::make_shared<std::atomic<TaskIndex>>(-1);
    auto coro = make_schedule_coro(std::move(task), executor_.get(), tid);
    executor_->enqueue_tracked(std::move(coro), std::move(name), tid);
}

coro::Coro Runtime::make_submit_coro(
    coro::CoroTask<void> task, std::shared_ptr<SubmitResult> result,
    Executor* exec, std::shared_ptr<std::atomic<TaskIndex>> tid) {
    try {
        co_await std::move(task);
        exec->mark_coro_completed(tid->load(std::memory_order_acquire));
    } catch (...) {
        exec->mark_coro_completed(tid->load(std::memory_order_acquire));
        result->exception = std::current_exception();
    }
    result->signal.set_value();
    result.reset();
}

coro::Coro Runtime::make_schedule_coro(
    coro::CoroTask<void> task, Executor* exec,
    std::shared_ptr<std::atomic<TaskIndex>> tid) {
    try {
        co_await std::move(task);
        exec->mark_coro_completed(tid->load(std::memory_order_acquire));
    } catch (const std::exception& e) {
        exec->mark_coro_completed(tid->load(std::memory_order_acquire));
        DFTRACER_UTILS_LOG_ERROR("schedule() task threw: %s", e.what());
    } catch (...) {
        exec->mark_coro_completed(tid->load(std::memory_order_acquire));
        DFTRACER_UTILS_LOG_ERROR("%s",
                                 "schedule() task threw unknown exception");
    }
}

ExecutorProgress Runtime::get_progress() const {
    return executor_->get_progress();
}

bool Runtime::is_responsive() const { return executor_->is_responsive(); }

void Runtime::set_global_timeout(std::chrono::milliseconds timeout) {
    watchdog_->set_global_timeout(timeout);
}

void Runtime::set_default_task_timeout(std::chrono::milliseconds timeout) {
    watchdog_->set_default_task_timeout(timeout);
}

void Runtime::shutdown() {
    bool expected = false;
    if (!shutdown_called_.compare_exchange_strong(expected, true)) return;
    if (watchdog_) watchdog_->stop();
    if (executor_) executor_->shutdown();
}

std::size_t Runtime::threads() const { return threads_; }

}  // namespace dftracer::utils
