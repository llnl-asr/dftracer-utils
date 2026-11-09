// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/memory_config.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/scheduler/task_executor.h>
#include <dftracer/utils/runtime/scheduler/watchdog.h>
#include <dftracer/utils/runtime/spawn_detached.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>

namespace dftracer::utils {

namespace {
// Resolve the worker-thread count. DFTRACER_UTILS_THREADS overrides everything
// (a debugging lever: force a fixed count, e.g. 1 for a single-threaded async
// loop). Otherwise 0 means hardware_concurrency.
std::size_t resolve_threads(std::size_t requested) {
    if (auto env = common::Env::get<dstd::StringView>("DFTRACER_UTILS_THREADS");
        env.has_value()) {
        if (auto n = dstd::from_chars<long long>(*env); n && *n > 0) {
            return static_cast<std::size_t>(*n);
        }
    }
    return requested == 0 ? common::hardware_concurrency() : requested;
}

// Resolve the I/O-pool size. DFTRACER_UTILS_IO_THREADS overrides everything (a
// lever to bound the epoll/kqueue thread pool independently of the compute
// pool, e.g. so the coordinator does not open a full-machine-sized pool).
// Otherwise 0 means one I/O thread per worker.
std::size_t resolve_io_threads(std::size_t requested, std::size_t workers) {
    if (auto env =
            common::Env::get<dstd::StringView>("DFTRACER_UTILS_IO_THREADS");
        env.has_value()) {
        if (auto n = dstd::from_chars<long long>(*env); n && *n > 0) {
            return static_cast<std::size_t>(*n);
        }
    }
    return requested == 0 ? workers : requested;
}

// A Runtime constructor has no way to report failure, so a runtime that cannot
// start ends the process here, with the cause.
void start_or_fatal(TaskExecutor& executor) {
    auto started = executor.start();
    if (!started) dstd::fatal("runtime: cannot start", started.error().c_str());
}
}  // namespace

Runtime::Runtime(std::size_t threads) : threads_(resolve_threads(threads)) {
    common::unwrap(common::configure_memory_from_env());
    ExecutorConfig config;
    config.thread_count = threads_.load(std::memory_order_relaxed);
    config.io_pool_size = resolve_io_threads(
        config.io_pool_size, threads_.load(std::memory_order_relaxed));
    executor_ = make_task_executor(config);
    start_or_fatal(*executor_);

    watchdog_ = dstd::make_unique<Watchdog>();
    watchdog_->set_executor(executor_.get());
}

Runtime::Runtime(const ExecutorConfig& config, bool enable_watchdog)
    : threads_(resolve_threads(config.thread_count)),
      io_backend_(config.io_backend_type),
      io_batch_threshold_(config.io_batch_threshold) {
    common::unwrap(common::configure_memory_from_env());
    ExecutorConfig cfg = config;
    cfg.thread_count = threads_.load(std::memory_order_relaxed);
    cfg.io_pool_size = resolve_io_threads(
        config.io_pool_size, threads_.load(std::memory_order_relaxed));
    executor_ = make_task_executor(cfg);
    start_or_fatal(*executor_);

    if (enable_watchdog) {
        watchdog_ = dstd::make_unique<Watchdog>();
        watchdog_->set_executor(executor_.get());
    }
}

Runtime::Runtime(const ExecutorConfig& config,
                 dstd::UniquePtr<Watchdog> watchdog)
    : threads_(resolve_threads(config.thread_count)),
      io_backend_(config.io_backend_type),
      io_batch_threshold_(config.io_batch_threshold) {
    ExecutorConfig cfg = config;
    cfg.thread_count = threads_.load(std::memory_order_relaxed);
    cfg.io_pool_size = resolve_io_threads(
        config.io_pool_size, threads_.load(std::memory_order_relaxed));
    executor_ = make_task_executor(cfg);
    start_or_fatal(*executor_);

    watchdog_ = std::move(watchdog);
    if (watchdog_) {
        watchdog_->set_executor(executor_.get());
    }
}

Runtime::~Runtime() { stop(); }

TaskHandle Runtime::submit(coro::CoroTask<void> task, dstd::StringView name) {
    if (shutdown_called_.load(std::memory_order_acquire)) {
        throw common::DFTUtilsException(common::ErrorCode::Pipeline,
                                        "Runtime is shut down");
    }
    // The handle, its shared state and the futures list outlive the
    // submitting thread's pool scope, so they charge the general pool.
    dstd::String task_name;
    dstd::SharedPtr<dstd::Promise<void>> promise;
    dstd::SharedFuture<void> future;
    dstd::SharedPtr<std::atomic<common::TaskIndex>> tid;
    {
        memory::detail::Scope bookkeeping_scope{
            memory::detail::Manager::get().general()};
        if (name.empty()) {
            task_name = "task-" + dstd::to_string(task_name_counter_++);
        } else {
            task_name.assign(name.data(), name.size());
        }
        promise = dstd::make_shared<dstd::Promise<void>>();
        future = promise->get_future().share();
        tid = dstd::make_shared<std::atomic<common::TaskIndex>>(-1);
    }

    auto wrapper = [](coro::CoroTask<void> t,
                      dstd::SharedPtr<dstd::Promise<void>> p,
                      TaskExecutor* exec,
                      dstd::SharedPtr<std::atomic<common::TaskIndex>> task_id)
        -> coro::detail::Coro {
        exec->mark_coro_started(task_id->load(std::memory_order_acquire));
        try {
            co_await std::move(t);
            t = coro::CoroTask<void>{
                std::coroutine_handle<coro::CoroTask<void>::promise_type>{}};
            exec->mark_coro_completed(task_id->load(std::memory_order_acquire));
        } catch (...) {
            t = coro::CoroTask<void>{
                std::coroutine_handle<coro::CoroTask<void>::promise_type>{}};
            mark_coro_failed(*exec, task_id->load(std::memory_order_acquire),
                             std::current_exception());
            p->set_exception(std::current_exception());
            co_return;
        }
        p->set_value();
    };

    // Set the executor on the task's promise so awaitables (e.g. channels)
    // that capture `get_root_promise()->get_executor()` can schedule
    // resumption. Without this, awaiters end up with executor=nullptr because
    // the wrapping `coro::detail::Coro` doesn't extend PromiseBase and the
    // root-promise chain stops at the user's CoroTask.
    if (task.handle()) {
        task.handle().promise().set_executor(executor_.get());
    }
    auto coro = wrapper(std::move(task), promise, executor_.get(), tid);
    common::TaskIndex id =
        executor_->enqueue_tracked(std::move(coro), task_name, tid);

    {
        memory::detail::Scope bookkeeping_scope{
            memory::detail::Manager::get().general()};
        std::lock_guard<std::mutex> lock(futures_mutex_);
        cleanup_completed_futures();
        outstanding_futures_.push_back(future);
    }

    return TaskHandle{future, id, std::move(task_name)};
}

void Runtime::wait_all() {
    dstd::Vector<dstd::SharedFuture<void>> futures;
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
                       [](const dstd::SharedFuture<void>& f) {
                           return f.wait_for(std::chrono::seconds(0)) ==
                                  std::future_status::ready;
                       }),
        outstanding_futures_.end());
}

ExecutorProgress Runtime::progress() const { return executor_->progress(); }

bool Runtime::is_responsive() const { return executor_->is_responsive(); }

dstd::Result<void> Runtime::set_global_timeout(
    std::chrono::milliseconds timeout) {
    if (!watchdog_) {
        return common::make_error(
            common::ErrorCode::Pipeline,
            "Cannot set timeout: Runtime created without watchdog");
    }
    watchdog_->set_global_timeout(timeout);
    return {};
}

dstd::Result<void> Runtime::set_default_task_timeout(
    std::chrono::milliseconds timeout) {
    if (!watchdog_) {
        return common::make_error(
            common::ErrorCode::Pipeline,
            "Cannot set timeout: Runtime created without watchdog");
    }
    watchdog_->set_default_task_timeout(timeout);
    return {};
}

void Runtime::shutdown() {
    if (process_runtime_) {
        DFTRACER_UTILS_LOG_DEBUG(
            "%s",
            "shutdown() of the process runtime ignored; it stops at exit");
        return;
    }
    stop();
}

void Runtime::configure(const RuntimeSettings& settings) {
    if (settings.io_backend != runtime::io::IoBackendType::Auto &&
        settings.io_backend != io_backend_) {
        DFTRACER_UTILS_LOG_WARN("%s",
                                "runtime: the I/O backend is fixed when the "
                                "runtime is made; the request is ignored");
    }
    if (settings.io_batch_threshold != 0 &&
        settings.io_batch_threshold != io_batch_threshold_) {
        DFTRACER_UTILS_LOG_WARN(
            "%s",
            "runtime: the I/O batch threshold is fixed when "
            "the runtime is made; the request is ignored");
    }
    if (settings.threads != 0 || settings.eager.has_value()) {
        const std::size_t threads =
            settings.threads != 0 ? resolve_threads(settings.threads)
                                  : threads_.load(std::memory_order_acquire);
        executor_->set_threads(threads, settings.eager);
        threads_.store(executor_->thread_count(), std::memory_order_release);
    }
    if (settings.io_threads != 0)
        executor_->grow_io_threads(resolve_io_threads(
            settings.io_threads, threads_.load(std::memory_order_acquire)));
    if (settings.deadlock_timeout)
        executor_->set_deadlock_timeout(*settings.deadlock_timeout);
}

void Runtime::stop() {
    bool expected = false;
    if (!shutdown_called_.compare_exchange_strong(expected, true)) return;
    if (watchdog_) watchdog_->stop();
    if (executor_) executor_->shutdown();
    std::lock_guard<std::mutex> lock(futures_mutex_);
    outstanding_futures_.clear();
}

std::size_t Runtime::threads() const {
    return threads_.load(std::memory_order_acquire);
}

std::size_t Runtime::io_threads() const {
    return executor_ ? executor_->io_pool_size() : 0;
}

namespace {
std::mutex g_default_mtx;
dstd::SharedPtr<Runtime> g_default_runtime;
// Set once the process runtime stopped at exit, so a late blocking get() does
// not make a new one.
std::atomic<bool> g_default_stopped{false};

// A fork copies the process runtime without its threads. The child drops the
// copy unused, never touching its executor, and makes its own on first use.
DFTRACER_UTILS_NO_THREAD_SAFETY_ANALYSIS void lock_default_for_fork() {
    g_default_mtx.lock();
}
DFTRACER_UTILS_NO_THREAD_SAFETY_ANALYSIS void unlock_default_after_fork() {
    g_default_mtx.unlock();
}
DFTRACER_UTILS_NO_THREAD_SAFETY_ANALYSIS void drop_inherited_default() {
    if (g_default_runtime) {
        static auto* inherited = new dstd::Vector<dstd::SharedPtr<Runtime>>();
        inherited->push_back(std::move(g_default_runtime));
    }
    g_default_mtx.unlock();
}

[[maybe_unused]] const bool FORK_HANDLER_REGISTERED =
    ::pthread_atfork(lock_default_for_fork, unlock_default_after_fork,
                     drop_inherited_default) == 0;

ExecutorConfig config_of(const RuntimeSettings& settings) {
    ExecutorConfig cfg;
    cfg.thread_count = settings.threads;
    cfg.io_pool_size = settings.io_threads;
    const bool eager = settings.eager.value_or(!elastic_default_enabled());
    cfg.min_workers = eager ? 0 : 1;
    if (settings.deadlock_timeout)
        cfg.deadlock_timeout = *settings.deadlock_timeout;
    cfg.io_backend_type = settings.io_backend;
    if (settings.io_batch_threshold != 0)
        cfg.io_batch_threshold = settings.io_batch_threshold;
    return cfg;
}
}  // namespace

void Runtime::become_process_runtime() {
    process_runtime_ = true;
    [[maybe_unused]] static const bool hooked =
        (dstd::add_shutdown_hook(dstd::ShutdownStage::StopRuntime,
                                 &Runtime::stop_at_exit),
         true);
}

void Runtime::stop_at_exit() {
    dstd::SharedPtr<Runtime> rt;
    {
        std::lock_guard<std::mutex> lock(g_default_mtx);
        rt = std::move(g_default_runtime);
        g_default_runtime = nullptr;
        g_default_stopped.store(true, std::memory_order_release);
    }
    // Shutdown stops the runtime before the memory exit check reads the
    // counters, which lets the workers free their last frames first. The
    // static gives up its reference too, so the runtime and the executor it
    // owns are freed before that check.
    if (rt) {
        rt->stop();
        rt.reset();
    }
}

bool elastic_default_enabled() {
    if (auto e = common::Env::get<dstd::StringView>("DFTRACER_UTILS_ELASTIC");
        e.has_value()) {
        return !(*e == "0" || *e == "false" || *e == "off");
    }
    return true;
}

dstd::SharedPtr<Runtime> default_runtime_shared() {
    std::lock_guard<std::mutex> lock(g_default_mtx);
    if (!g_default_runtime) {
        g_default_runtime = dstd::make_shared<Runtime>(config_of({}));
        g_default_runtime->become_process_runtime();
    }
    return g_default_runtime;
}

Runtime& default_runtime() { return *default_runtime_shared(); }

namespace {

dstd::SharedPtr<coro::Executor> process_executor() {
    if (g_default_stopped.load(std::memory_order_acquire)) return nullptr;
    // The first blocking get() may make the runtime; its memory belongs to
    // the process, not to the pool of the task that happened to ask.
    memory::detail::Scope scope{memory::detail::Manager::get().general()};
    dstd::SharedPtr<Runtime> rt = default_runtime_shared();
    TaskExecutor* executor = rt->executor();
    if (executor == nullptr || !executor->is_running()) return nullptr;
    return dstd::SharedPtr<coro::Executor>(rt, executor);
}

[[maybe_unused]] const bool BLOCKING_EXECUTOR_SET =
    (coro::set_blocking_executor(&process_executor), true);

}  // namespace

Runtime& configure_default_runtime(const RuntimeSettings& settings) {
    std::lock_guard<std::mutex> lock(g_default_mtx);
    if (!g_default_runtime) {
        g_default_runtime = dstd::make_shared<Runtime>(config_of(settings));
        g_default_runtime->become_process_runtime();
        return *g_default_runtime;
    }
    g_default_runtime->configure(settings);
    return *g_default_runtime;
}

dstd::SharedPtr<Runtime> peek_default_runtime() {
    std::lock_guard<std::mutex> lock(g_default_mtx);
    return g_default_runtime;
}

namespace {

bool executor_stopping(coro::Executor* executor) noexcept {
    if (!executor) return true;
    if (auto* runtime_executor = dynamic_cast<TaskExecutor*>(executor)) {
        if (!runtime_executor->is_running()) return true;
    }
    auto token = executor->shutdown_token();
    return token && token->load(std::memory_order_acquire);
}

}  // namespace

bool spawn_detached(coro::Executor* executor, coro::CoroTask<void> task) {
    if (executor_stopping(executor)) return false;
    auto make_coro = [](coro::CoroTask<void> t,
                        coro::Executor* exec) -> coro::detail::Coro {
        try {
            t.handle().promise().set_executor(exec);
            co_await std::move(t);
        } catch (...) {
            DFTRACER_UTILS_LOG_ERROR("detached task failed");
        }
    };
    auto c = make_coro(std::move(task), executor);
    c.handle().promise().executor = executor;
    auto handle = c.release();
    executor->post(handle);
    return true;
}

}  // namespace dftracer::utils
