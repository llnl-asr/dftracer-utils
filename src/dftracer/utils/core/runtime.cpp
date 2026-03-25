#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/runtime.h>

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace dftracer::utils {

Runtime::Runtime(std::size_t threads)
    : threads_(threads == 0 ? dftracer_utils_hardware_concurrency() : threads) {
    ExecutorConfig config;
    config.num_threads = threads_;
    executor_ = std::make_unique<Executor>(config);
    executor_->start();

    watchdog_ = std::make_unique<Watchdog>();
    watchdog_->set_executor(executor_.get());
}

Runtime::Runtime(const ExecutorConfig& config, bool enable_watchdog)
    : threads_(config.num_threads == 0 ? dftracer_utils_hardware_concurrency()
                                       : config.num_threads) {
    executor_ = std::make_unique<Executor>(config);
    executor_->start();

    if (enable_watchdog) {
        watchdog_ = std::make_unique<Watchdog>();
        watchdog_->set_executor(executor_.get());
    }
}

Runtime::Runtime(const ExecutorConfig& config,
                 std::unique_ptr<Watchdog> watchdog)
    : threads_(config.num_threads == 0 ? dftracer_utils_hardware_concurrency()
                                       : config.num_threads) {
    executor_ = std::make_unique<Executor>(config);
    executor_->start();

    watchdog_ = std::move(watchdog);
    if (watchdog_) {
        watchdog_->set_executor(executor_.get());
    }
}

Runtime::~Runtime() { shutdown(); }

TaskHandle Runtime::submit(coro::CoroTask<void> task, std::string name) {
    if (shutdown_called_.load(std::memory_order_acquire)) {
        throw std::runtime_error("Runtime is shut down");
    }
    if (name.empty()) {
        name = "task-" + std::to_string(task_name_counter_++);
    }

    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future().share();
    auto tid = std::make_shared<std::atomic<TaskIndex>>(-1);

    auto wrapper =
        [](coro::CoroTask<void> t, std::shared_ptr<std::promise<void>> p,
           Executor* exec,
           std::shared_ptr<std::atomic<TaskIndex>> task_id) -> coro::Coro {
        try {
            co_await std::move(t);
            exec->mark_coro_completed(task_id->load(std::memory_order_acquire));
        } catch (...) {
            exec->mark_coro_completed(task_id->load(std::memory_order_acquire));
            p->set_exception(std::current_exception());
            co_return;
        }
        p->set_value();
    };

    auto coro = wrapper(std::move(task), promise, executor_.get(), tid);
    TaskIndex id = executor_->enqueue_tracked(std::move(coro), name, tid);

    {
        std::lock_guard<std::mutex> lock(futures_mutex_);
        cleanup_completed_futures();
        outstanding_futures_.push_back(future);
    }

    return TaskHandle{future, id, std::move(name)};
}

void Runtime::wait_all() {
    std::vector<std::shared_future<void>> futures;
    {
        std::lock_guard<std::mutex> lock(futures_mutex_);
        futures = std::move(outstanding_futures_);
        outstanding_futures_.clear();
    }
    for (auto& f : futures) {
        f.wait();
    }
}

void Runtime::cleanup_completed_futures() {
    outstanding_futures_.erase(
        std::remove_if(outstanding_futures_.begin(), outstanding_futures_.end(),
                       [](const std::shared_future<void>& f) {
                           return f.wait_for(std::chrono::seconds(0)) ==
                                  std::future_status::ready;
                       }),
        outstanding_futures_.end());
}

ExecutorProgress Runtime::get_progress() const {
    return executor_->get_progress();
}

bool Runtime::is_responsive() const { return executor_->is_responsive(); }

void Runtime::set_global_timeout(std::chrono::milliseconds timeout) {
    if (!watchdog_) {
        throw std::runtime_error(
            "Cannot set timeout: Runtime created without watchdog");
    }
    watchdog_->set_global_timeout(timeout);
}

void Runtime::set_default_task_timeout(std::chrono::milliseconds timeout) {
    if (!watchdog_) {
        throw std::runtime_error(
            "Cannot set timeout: Runtime created without watchdog");
    }
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
