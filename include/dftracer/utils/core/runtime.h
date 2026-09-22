#ifndef DFTRACER_UTILS_CORE_RUNTIME_H
#define DFTRACER_UTILS_CORE_RUNTIME_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/task_handle.h>
#include <dftracer/utils/core/tasks/coro_scope.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace dftracer::utils {

class Watchdog;

namespace detail {

/// Thread-local flag marking that the current thread is executing inside a
/// parallel_for/parallel_reduce chunk, so a nested call runs serial (one level
/// of data parallelism, no fork-join inside a fork-join).
inline bool& parallel_region_flag() {
    thread_local bool flag = false;
    return flag;
}
inline bool in_parallel_region() { return parallel_region_flag(); }
struct ParallelRegionGuard {
    bool prev;
    ParallelRegionGuard() : prev(parallel_region_flag()) {
        parallel_region_flag() = true;
    }
    ~ParallelRegionGuard() { parallel_region_flag() = prev; }
};

}  // namespace detail

/// Lightweight wrapper around Executor + Watchdog for running coroutines
/// on a thread pool without Pipeline/Scheduler/DAG overhead.
/// Intended for Python bindings and other non-DAG consumers.
class Runtime {
   public:
    /// The worker-thread count can be overridden at runtime by the
    /// DFTRACER_UTILS_THREADS environment variable (takes precedence over the
    /// requested count; e.g. set it to 1 for a single-threaded async loop when
    /// debugging). 0/unset means hardware_concurrency.
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

    /// Submit a scoped task (provides CoroScope to the lambda).
    /// Returns immediately, task runs on executor.
    ///
    /// Usage:
    /// @code
    /// auto handle = rt->scope("my_task", [](CoroScope& scope) ->
    /// CoroTask<void> {
    ///     scope.spawn([](CoroScope& s) -> CoroTask<void> { co_return; });
    ///     co_await scope.join();
    /// });
    /// handle.get();  // wait when needed
    /// @endcode
    template <typename Func>
        requires std::is_invocable_r_v<coro::CoroTask<void>, Func, CoroScope&>
    TaskHandle scope(std::string name, Func&& func) {
        return submit(run_coro_scope(executor_.get(), std::forward<Func>(func)),
                      std::move(name));
    }

    /// Run a void coroutine scope to completion, blocking the caller and
    /// re-raising any exception. Called from one of this runtime's own workers
    /// it hands off the worker's run slot for the wait (lazy: reclaimed with a
    /// CAS on a short block, by the sysmon on a long one), so parking the
    /// caller cannot starve the pool the work runs on.
    template <typename Func>
        requires std::is_invocable_r_v<coro::CoroTask<void>, Func, CoroScope&>
    void run_blocking(std::string name, Func&& func) {
        const bool reentrant = Executor::current() == executor_.get();
        BlockingRegion block(reentrant ? executor_.get() : nullptr);
        scope(std::move(name), std::forward<Func>(func)).wait();
    }

    /// Data-parallel fork-join over [0, n): split into `grain`-sized chunks and
    /// run `body(begin, end)` on the pool, blocking until all finish. A worker
    /// pool of min(chunks, threads()) coroutines drains an atomic chunk
    /// counter, so scheduling is dynamic (load-balanced) and allocation is
    /// O(threads), not O(chunks). Runs inline serial for a single chunk or when
    /// already inside a parallel region (no nested fork-join). Safe to call
    /// from a worker thread (the run_blocking handoff keeps the pool
    /// saturated). `body` must be safe to run concurrently on disjoint
    /// sub-ranges.
    template <typename Body>
        requires std::is_invocable_v<Body, std::int64_t, std::int64_t>
    void parallel_for(std::int64_t n, std::int64_t grain, Body body) {
        if (n <= 0) return;
        const std::int64_t chunks = (n + grain - 1) / grain;
        if (chunks <= 1 || detail::in_parallel_region()) {
            body(0, n);
            return;
        }
        const std::int64_t workers = std::min<std::int64_t>(
            chunks, static_cast<std::int64_t>(threads()));
        std::atomic<std::int64_t> next{0};
        run_blocking("parallel_for", [&](CoroScope& s) -> coro::CoroTask<void> {
            for (std::int64_t w = 0; w < workers; ++w)
                s.spawn([&](CoroScope&) -> coro::CoroTask<void> {
                    detail::ParallelRegionGuard guard;
                    for (std::int64_t i; (i = next.fetch_add(1)) < chunks;)
                        body(i * grain, std::min(n, (i + 1) * grain));
                    co_return;
                });
            co_await s.join();
        });
    }

    /// Data-parallel reduction over [0, n): `map(begin, end)` produces a
    /// partial per range, combined pairwise by `combine` (associative). Each
    /// worker folds the chunks it drains into a local accumulator, then the
    /// per-worker partials are combined. `T` must be copyable (for the identity
    /// seed).
    template <typename T, typename Map, typename Combine>
    T parallel_reduce(std::int64_t n, std::int64_t grain, T identity, Map map,
                      Combine combine) {
        if (n <= 0) return identity;
        const std::int64_t chunks = (n + grain - 1) / grain;
        if (chunks <= 1 || detail::in_parallel_region())
            return combine(std::move(identity), map(0, n));
        const std::int64_t workers = std::min<std::int64_t>(
            chunks, static_cast<std::int64_t>(threads()));
        std::atomic<std::int64_t> next{0};
        std::vector<T> partials(static_cast<std::size_t>(workers), identity);
        run_blocking(
            "parallel_reduce", [&](CoroScope& s) -> coro::CoroTask<void> {
                for (std::int64_t w = 0; w < workers; ++w)
                    s.spawn([&, w](CoroScope&) -> coro::CoroTask<void> {
                        detail::ParallelRegionGuard guard;
                        T acc = identity;
                        for (std::int64_t i; (i = next.fetch_add(1)) < chunks;)
                            acc = combine(
                                std::move(acc),
                                map(i * grain, std::min(n, (i + 1) * grain)));
                        partials[static_cast<std::size_t>(w)] = std::move(acc);
                        co_return;
                    });
                co_await s.join();
            });
        T acc = std::move(identity);
        for (T& p : partials) acc = combine(std::move(acc), std::move(p));
        return acc;
    }

    /// Wait for all outstanding tasks to complete.
    void wait_all();

    ExecutorProgress get_progress() const;
    bool is_responsive() const;

    void set_global_timeout(std::chrono::milliseconds timeout);
    void set_default_task_timeout(std::chrono::milliseconds timeout);

    void shutdown();
    std::size_t threads() const;
    std::size_t io_threads() const;
    TaskExecutor* executor() { return executor_.get(); }
    Watchdog* watchdog() { return watchdog_.get(); }

   private:
    void cleanup_completed_futures();

    std::unique_ptr<TaskExecutor> executor_;
    std::unique_ptr<Watchdog> watchdog_;
    std::size_t threads_;
    std::atomic<bool> shutdown_called_{false};
    std::atomic<uint64_t> task_name_counter_{0};
    std::vector<std::shared_future<void>> outstanding_futures_;
    std::mutex futures_mutex_;
};

/// The process-wide default Runtime.
/// `default_runtime()`/`default_runtime_shared` lazily create one
/// (hardware_concurrency) on first use; `peek_default_runtime` returns the
/// current one or null without creating; `set_default_runtime` installs an
/// explicit one (null resets to lazy). One shared default across the stack - a
/// caller wanting thread control installs or passes its own Runtime.
Runtime& default_runtime();
std::shared_ptr<Runtime> default_runtime_shared();

/// Whether elastic pools are enabled: true unless DFTRACER_UTILS_ELASTIC is a
/// false value ("0"/"false"/"off"). The master switch the test suites flip to
/// force eager, deterministic pools.
bool elastic_default_enabled();
std::shared_ptr<Runtime> peek_default_runtime();
void set_default_runtime(std::shared_ptr<Runtime> rt);

/// Install `rt` as the default only if no default is set or the current
/// default is the lazily-created fallback; a user-installed default (set here
/// or via set_default_runtime) is left in place. Returns true if `rt` became
/// the default.
bool try_install_default_runtime(std::shared_ptr<Runtime> rt);

/// Clear the default runtime if it is currently `rt` (identity check),
/// reverting later NULL-means-default lookups to the lazy fallback. No-op if
/// `rt` is not the current default.
void clear_default_runtime_if(Runtime* rt);

template <typename T>
TypedTaskHandle<T> Runtime::submit(coro::CoroTask<T> task, std::string name) {
    if (shutdown_called_.load(std::memory_order_acquire)) {
        throw DFTUtilsException(ErrorCode::PIPELINE, "Runtime is shut down");
    }
    if (name.empty()) {
        name = "task-" + std::to_string(task_name_counter_++);
    }

    auto typed_promise = std::make_shared<std::promise<std::shared_ptr<T>>>();
    auto typed_future = typed_promise->get_future().share();

    // void future for outstanding_futures_ tracking
    auto void_promise = std::make_shared<std::promise<void>>();
    auto void_future = void_promise->get_future().share();

    auto tid = std::make_shared<std::atomic<TaskIndex>>(-1);

    auto wrapper =
        [](coro::CoroTask<T> t,
           std::shared_ptr<std::promise<std::shared_ptr<T>>> tp,
           std::shared_ptr<std::promise<void>> vp, Executor* exec,
           std::shared_ptr<std::atomic<TaskIndex>> task_id) -> coro::Coro {
        try {
            T val = co_await std::move(t);
            t = coro::CoroTask<T>{std::coroutine_handle<
                typename coro::CoroTask<T>::promise_type>{}};
            exec->mark_coro_completed(task_id->load(std::memory_order_acquire));
            tp->set_value(std::make_shared<T>(std::move(val)));
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

    // Set the executor on the task's promise so awaitables (e.g. channels)
    // that capture `get_root_promise()->get_executor()` can schedule
    // resumption. Without this, awaiters end up with executor=nullptr because
    // the wrapping `coro::Coro` doesn't extend PromiseBase and the
    // root-promise chain stops at the user's CoroTask.
    if (task.handle()) {
        task.handle().promise().set_executor(executor_.get());
    }
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
