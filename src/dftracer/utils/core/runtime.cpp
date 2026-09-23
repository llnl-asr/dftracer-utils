#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/env.h>
#include <dftracer/utils/core/pipeline/watchdog.h>
#include <dftracer/utils/core/runtime.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace dftracer::utils {

namespace {
// Resolve the worker-thread count. DFTRACER_UTILS_THREADS overrides everything
// (a debugging lever: force a fixed count, e.g. 1 for a single-threaded async
// loop). Otherwise 0 means hardware_concurrency.
std::size_t resolve_threads(std::size_t requested) {
    if (auto env = Env::get<std::string_view>("DFTRACER_UTILS_THREADS");
        env.has_value()) {
        const long long n =
            std::strtoll(std::string(*env).c_str(), nullptr, 10);
        if (n > 0) return static_cast<std::size_t>(n);
    }
    return requested == 0 ? hardware_concurrency() : requested;
}

// Resolve the I/O-pool size. DFTRACER_UTILS_IO_THREADS overrides everything (a
// lever to bound the epoll/kqueue thread pool independently of the compute
// pool, e.g. so the coordinator does not open a full-machine-sized pool).
// Otherwise 0 means hardware_concurrency, matching the executor default.
std::size_t resolve_io_threads(std::size_t requested) {
    if (auto env = Env::get<std::string_view>("DFTRACER_UTILS_IO_THREADS");
        env.has_value()) {
        const long long n =
            std::strtoll(std::string(*env).c_str(), nullptr, 10);
        if (n > 0) return static_cast<std::size_t>(n);
    }
    return requested == 0 ? hardware_concurrency() : requested;
}
}  // namespace

Runtime::Runtime(std::size_t threads) : threads_(resolve_threads(threads)) {
    ExecutorConfig config;
    config.num_threads = threads_;
    config.io_pool_size = resolve_io_threads(config.io_pool_size);
    executor_ = make_task_executor(config);
    executor_->start();

    watchdog_ = std::make_unique<Watchdog>();
    watchdog_->set_executor(executor_.get());
}

Runtime::Runtime(const ExecutorConfig& config, bool enable_watchdog)
    : threads_(resolve_threads(config.num_threads)) {
    ExecutorConfig cfg = config;
    cfg.num_threads = threads_;
    cfg.io_pool_size = resolve_io_threads(config.io_pool_size);
    executor_ = make_task_executor(cfg);
    executor_->start();

    if (enable_watchdog) {
        watchdog_ = std::make_unique<Watchdog>();
        watchdog_->set_executor(executor_.get());
    }
}

Runtime::Runtime(const ExecutorConfig& config,
                 std::unique_ptr<Watchdog> watchdog)
    : threads_(resolve_threads(config.num_threads)) {
    ExecutorConfig cfg = config;
    cfg.num_threads = threads_;
    cfg.io_pool_size = resolve_io_threads(config.io_pool_size);
    executor_ = make_task_executor(cfg);
    executor_->start();

    watchdog_ = std::move(watchdog);
    if (watchdog_) {
        watchdog_->set_executor(executor_.get());
    }
}

Runtime::~Runtime() { shutdown(); }

TaskHandle Runtime::submit(coro::CoroTask<void> task, std::string name) {
    if (shutdown_called_.load(std::memory_order_acquire)) {
        throw DFTUtilsException(ErrorCode::PIPELINE, "Runtime is shut down");
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
            t = coro::CoroTask<void>{
                std::coroutine_handle<coro::CoroTask<void>::promise_type>{}};
            exec->mark_coro_completed(task_id->load(std::memory_order_acquire));
        } catch (...) {
            t = coro::CoroTask<void>{
                std::coroutine_handle<coro::CoroTask<void>::promise_type>{}};
            exec->mark_coro_completed(task_id->load(std::memory_order_acquire));
            p->set_exception(std::current_exception());
            co_return;
        }
        p->set_value();
    };

    // Set the executor on the task's promise so awaitables (e.g. channels)
    // that capture `get_root_promise()->get_executor()` can schedule
    // resumption. Without this, awaiters end up with executor=nullptr because
    // the wrapping `coro::Coro` doesn't extend PromiseBase and the
    // root-promise chain stops at the user's CoroTask.
    if (task.handle()) {
        task.handle().promise().set_executor(executor_.get());
    }
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
        throw DFTUtilsException(
            ErrorCode::PIPELINE,
            "Cannot set timeout: Runtime created without watchdog");
    }
    watchdog_->set_global_timeout(timeout);
}

void Runtime::set_default_task_timeout(std::chrono::milliseconds timeout) {
    if (!watchdog_) {
        throw DFTUtilsException(
            ErrorCode::PIPELINE,
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

std::size_t Runtime::io_threads() const {
    return executor_ ? executor_->get_io_pool_size() : 0;
}

namespace {
std::mutex g_default_mtx;
std::shared_ptr<Runtime> g_default_runtime;
bool g_default_is_lazy = false;
}  // namespace

bool elastic_default_enabled() {
    if (auto e = Env::get<std::string_view>("DFTRACER_UTILS_ELASTIC");
        e.has_value()) {
        return !(*e == "0" || *e == "false" || *e == "off");
    }
    return true;
}

std::shared_ptr<Runtime> default_runtime_shared() {
    std::lock_guard<std::mutex> lock(g_default_mtx);
    if (!g_default_runtime) {
        // Elastic (grow on demand) so a mostly-idle shared default - a CLI
        // between queries, a Python session - does not squat hardware_
        // concurrency threads on a shared node. Latency-sensitive consumers
        // (the server) build their own eager runtime.
        ExecutorConfig cfg;
        cfg.min_workers = elastic_default_enabled() ? 1 : 0;
        g_default_runtime = std::make_shared<Runtime>(cfg);
        g_default_is_lazy = true;
    }
    return g_default_runtime;
}

Runtime& default_runtime() { return *default_runtime_shared(); }

std::shared_ptr<Runtime> peek_default_runtime() {
    std::lock_guard<std::mutex> lock(g_default_mtx);
    return g_default_runtime;
}

void set_default_runtime(std::shared_ptr<Runtime> rt) {
    std::lock_guard<std::mutex> lock(g_default_mtx);
    g_default_runtime = std::move(rt);
    g_default_is_lazy = false;
}

bool try_install_default_runtime(std::shared_ptr<Runtime> rt) {
    std::lock_guard<std::mutex> lock(g_default_mtx);
    if (g_default_runtime && !g_default_is_lazy) return false;
    g_default_runtime = std::move(rt);
    g_default_is_lazy = false;
    return true;
}

void clear_default_runtime_if(Runtime* rt) {
    std::lock_guard<std::mutex> lock(g_default_mtx);
    if (g_default_runtime.get() == rt) {
        g_default_runtime.reset();
        g_default_is_lazy = false;
    }
}

}  // namespace dftracer::utils
