// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_RUNTIME_H
#define DFTRACER_UTILS_RUNTIME_RUNTIME_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/runtime/scheduler/task_executor.h>
#include <dftracer/utils/runtime/task_handle.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>

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

/// What configure_default_runtime() applies. A field left at its default keeps
/// what the runtime has, or takes the default when the runtime is new.
struct RuntimeSettings {
    /// Worker cap; 0 keeps it (hardware_concurrency for a new runtime).
    /// DFTRACER_UTILS_THREADS, when set, wins over it.
    std::size_t threads = 0;
    /// I/O threads; 0 keeps them. A runtime only grows them.
    /// DFTRACER_UTILS_IO_THREADS, when set, wins over it.
    std::size_t io_threads = 0;
    /// Spawn the full pool at once (true) or grow it on demand (false);
    /// unset keeps the mode (elastic for a new runtime, unless
    /// DFTRACER_UTILS_ELASTIC is off).
    dstd::Optional<bool> eager;
    /// The stall after which the executor reports a deadlock.
    dstd::Optional<std::chrono::seconds> deadlock_timeout;
    /// Fixed when the runtime is made: a later different value keeps the
    /// runtime's and logs a warning.
    io::IoBackendType io_backend = io::IoBackendType::Auto;
    /// Fixed when the runtime is made, as io_backend; 0 is the default.
    unsigned io_batch_threshold = 0;
};

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
    Runtime(const ExecutorConfig& config, dstd::UniquePtr<Watchdog> watchdog);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    /// Async submit returns immediately, task runs on executor.
    TaskHandle submit(coro::CoroTask<void> task, dstd::StringView name = "");

    /// Async submit with typed result.
    template <typename T>
    TypedTaskHandle<T> submit(coro::CoroTask<T> task,
                              dstd::StringView name = "");

    /// Runs `task` as a tracked task and blocks until it ends, raising its
    /// error. Called from one of this runtime's own workers it hands off the
    /// worker's run slot for the wait, so the pool keeps its capacity.
    template <typename T>
    T block_on(coro::Named<coro::CoroTask<T>> task) {
        const bool reentrant = coro::Executor::current() == executor_.get();
        coro::BlockingRegion block(reentrant ? executor_.get() : nullptr);
        if constexpr (std::is_void_v<T>) {
            submit(std::move(task.value), task.name).wait();
        } else {
            return submit(std::move(task.value), task.name).get();
        }
    }

    template <typename T>
    T block_on(coro::CoroTask<T> task) {
        return block_on(coro::Named<coro::CoroTask<T>>{{}, std::move(task)});
    }

    /// Data-parallel fork-join over [0, n): split into `grain`-sized chunks and
    /// run `body(begin, end)` on the pool, blocking until all finish. A worker
    /// pool of min(chunks, threads()) coroutines drains an atomic chunk
    /// counter, so scheduling is dynamic (load-balanced) and allocation is
    /// O(threads), not O(chunks). Runs inline serial for a single chunk or when
    /// already inside a parallel region (no nested fork-join). Safe to call
    /// from a worker thread (the block_on handoff keeps the pool
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
        block_on(coro::named(
            "parallel_for",
            coro::with_scope([&](coro::Scope& s) -> coro::CoroTask<void> {
                for (std::int64_t w = 0; w < workers; ++w)
                    s.spawn([&]() -> coro::CoroTask<void> {
                        detail::ParallelRegionGuard guard;
                        for (std::int64_t i; (i = next.fetch_add(1)) < chunks;)
                            body(i * grain, std::min(n, (i + 1) * grain));
                        co_return;
                    });
                co_return;
            })));
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
        dstd::Vector<T> partials(static_cast<std::size_t>(workers), identity);
        block_on(coro::named(
            "parallel_reduce",
            coro::with_scope([&](coro::Scope& s) -> coro::CoroTask<void> {
                for (std::int64_t w = 0; w < workers; ++w)
                    s.spawn([&, w]() -> coro::CoroTask<void> {
                        detail::ParallelRegionGuard guard;
                        T acc = identity;
                        for (std::int64_t i; (i = next.fetch_add(1)) < chunks;)
                            acc = combine(
                                std::move(acc),
                                map(i * grain, std::min(n, (i + 1) * grain)));
                        partials[static_cast<std::size_t>(w)] = std::move(acc);
                        co_return;
                    });
                co_return;
            })));
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

    /// Stops the executor. A no-op on the process runtime, which only the
    /// shutdown hooks stop.
    void shutdown();
    /// Applies `settings` in place, while work runs. Thread safe.
    void configure(const RuntimeSettings& settings);
    /// Stops the process runtime. For a shutdown hook only: nothing may
    /// submit work after it.
    static void stop_at_exit();
    std::size_t threads() const;
    std::size_t io_threads() const;
    TaskExecutor* executor() { return executor_.get(); }
    Watchdog* watchdog() { return watchdog_.get(); }

   private:
    void cleanup_completed_futures();

    friend dstd::SharedPtr<Runtime> default_runtime_shared();
    friend Runtime& configure_default_runtime(const RuntimeSettings& settings);
    void become_process_runtime();
    void stop();

    dstd::UniquePtr<TaskExecutor> executor_;
    dstd::UniquePtr<Watchdog> watchdog_;
    std::atomic<std::size_t> threads_;
    io::IoBackendType io_backend_ = io::IoBackendType::Auto;
    unsigned io_batch_threshold_ = 0;
    bool process_runtime_ = false;
    std::atomic<bool> shutdown_called_{false};
    std::atomic<uint64_t> task_name_counter_{0};
    dstd::Vector<dstd::SharedFuture<void>> outstanding_futures_;
    std::mutex futures_mutex_;
};

/// The process runtime: one per process, made on first use and never replaced.
/// Safe from any thread. Only process exit stops it. A forked child makes its
/// own on first use, since the inherited copy has no threads.
Runtime& default_runtime();
dstd::SharedPtr<Runtime> default_runtime_shared();

/// Makes the process runtime with `settings`, or applies them to it in place.
/// Never makes a second runtime.
Runtime& configure_default_runtime(const RuntimeSettings& settings);

/// Whether elastic pools are enabled: true unless DFTRACER_UTILS_ELASTIC is a
/// false value ("0"/"false"/"off"). The master switch the test suites flip to
/// force eager, deterministic pools.
bool elastic_default_enabled();
/// The process runtime, or null before its first use.
dstd::SharedPtr<Runtime> peek_default_runtime();

template <typename T>
TypedTaskHandle<T> Runtime::submit(coro::CoroTask<T> task,
                                   dstd::StringView name) {
    if (shutdown_called_.load(std::memory_order_acquire)) {
        throw common::DFTUtilsException(common::ErrorCode::Pipeline,
                                        "Runtime is shut down");
    }
    // The handle, its shared state and the futures list outlive the
    // submitting thread's pool scope, so they charge the general pool.
    dstd::String task_name;
    dstd::SharedPtr<dstd::Promise<dstd::SharedPtr<T>>> typed_promise;
    dstd::SharedFuture<dstd::SharedPtr<T>> typed_future;
    dstd::SharedPtr<dstd::Promise<void>> void_promise;
    dstd::SharedFuture<void> void_future;
    dstd::SharedPtr<std::atomic<common::TaskIndex>> tid;
    {
        memory::detail::Scope bookkeeping_scope{
            memory::detail::Manager::get().general()};
        if (name.empty()) {
            task_name = "task-" + dstd::to_string(task_name_counter_++);
        } else {
            task_name.assign(name.data(), name.size());
        }
        typed_promise = dstd::make_shared<dstd::Promise<dstd::SharedPtr<T>>>();
        typed_future = typed_promise->get_future().share();
        // void future for outstanding_futures_ tracking
        void_promise = dstd::make_shared<dstd::Promise<void>>();
        void_future = void_promise->get_future().share();
        tid = dstd::make_shared<std::atomic<common::TaskIndex>>(-1);
    }

    auto wrapper = [](coro::CoroTask<T> t,
                      dstd::SharedPtr<dstd::Promise<dstd::SharedPtr<T>>> tp,
                      dstd::SharedPtr<dstd::Promise<void>> vp,
                      TaskExecutor* exec,
                      dstd::SharedPtr<std::atomic<common::TaskIndex>> task_id)
        -> coro::detail::Coro {
        try {
            T val = co_await std::move(t);
            t = coro::CoroTask<T>{std::coroutine_handle<
                typename coro::CoroTask<T>::promise_type>{}};
            exec->mark_coro_completed(task_id->load(std::memory_order_acquire));
            tp->set_value(dstd::make_shared<T>(std::move(val)));
        } catch (...) {
            t = coro::CoroTask<T>{std::coroutine_handle<
                typename coro::CoroTask<T>::promise_type>{}};
            mark_coro_failed(*exec, task_id->load(std::memory_order_acquire),
                             std::current_exception());
            tp->set_exception(std::current_exception());
            vp->set_value();
            co_return;
        }
        vp->set_value();
    };

    // Set the executor on the task's promise so awaitables (e.g. channels)
    // that capture `get_root_promise()->get_executor()` can schedule
    // resumption. Without this, awaiters end up with executor=nullptr because
    // the wrapping `coro::detail::Coro` doesn't extend PromiseBase and the
    // root-promise chain stops at the user's CoroTask.
    if (task.handle()) {
        task.handle().promise().set_executor(executor_.get());
    }
    auto coro = wrapper(std::move(task), typed_promise, void_promise,
                        executor_.get(), tid);
    common::TaskIndex id =
        executor_->enqueue_tracked(std::move(coro), task_name, tid);

    {
        memory::detail::Scope bookkeeping_scope{
            memory::detail::Manager::get().general()};
        std::lock_guard<std::mutex> lock(futures_mutex_);
        cleanup_completed_futures();
        outstanding_futures_.push_back(void_future);
    }

    return TypedTaskHandle<T>{typed_future, id, std::move(task_name)};
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_RUNTIME_H
