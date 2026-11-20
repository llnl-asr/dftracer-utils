// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_RUNTIME_H
#define DFTRACER_UTILS_RUNTIME_RUNTIME_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/future.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
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
/// of data parallelism, no fork-join inside a fork-join). Defined out of line
/// so every library sees one flag per thread.
bool& parallel_region_flag();
inline bool in_parallel_region() { return parallel_region_flag(); }
struct ParallelRegionGuard {
    bool prev;
    ParallelRegionGuard() : prev(parallel_region_flag()) {
        parallel_region_flag() = true;
    }
    ~ParallelRegionGuard() { parallel_region_flag() = prev; }
};

/// State of one parallel_for/parallel_reduce call that a helper can outlive.
/// A helper counts itself in `in_flight` before it takes a chunk and touches
/// the caller's data only after it took one; the caller returns once the
/// chunks are gone and `in_flight` is zero, so a helper that starts later
/// finds no chunk and never reads the caller's frame.
struct ForkJoin {
    std::atomic<std::int64_t> next{0};
    std::atomic<std::int64_t> in_flight{0};
    std::int64_t chunks;

    explicit ForkJoin(std::int64_t chunk_count) : chunks(chunk_count) {}

    /// Takes the next chunk into @p chunk; false when none is left.
    bool claim(std::int64_t& chunk) {
        chunk = next.fetch_add(1);
        return chunk < chunks;
    }

    void wait_helpers() {
        for (std::int64_t v; (v = in_flight.load()) != 0;) in_flight.wait(v);
    }
};

/// Counts itself in flight before it claims a chunk, and reads @p work only
/// after it holds one.
template <typename Work>
coro::detail::Coro fork_join_helper(dstd::SharedPtr<ForkJoin> state, Work* work,
                                    std::int64_t slot) {
    state->in_flight.fetch_add(1);
    std::int64_t first;
    if (state->claim(first)) work->run(slot, first, *state);
    if (state->in_flight.fetch_sub(1) == 1) state->in_flight.notify_all();
    co_return;
}

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
    runtime::io::IoBackendType io_backend = runtime::io::IoBackendType::Auto;
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

    /// Runs `task` as a tracked task and blocks until it ends. Gives its value
    /// as a @ref coro::TaskResult, which holds dstd::Condition::Cancelled when
    /// the task was stopped, or the error the task ended with. Called
    /// from one of this runtime's own workers it hands off the
    /// worker's run slot for the wait, so the pool keeps its capacity.
    template <typename T>
    coro::TaskResult<T> block_on(coro::Named<coro::CoroTask<T>> task) {
        const bool reentrant = coro::Executor::current() == executor_.get();
        coro::BlockingRegion block(reentrant ? executor_.get() : nullptr);
        return submit(std::move(task.value), task.name).get();
    }

    template <typename T>
    coro::TaskResult<T> block_on(coro::CoroTask<T> task) {
        return block_on(coro::Named<coro::CoroTask<T>>{{}, std::move(task)});
    }

    /// Data-parallel fork-join over [0, n): split into `grain`-sized chunks and
    /// run `body(begin, end)`, blocking until all finish. The calling thread
    /// and up to min(chunks, threads()) - 1 helpers drain an atomic chunk
    /// counter, so scheduling is dynamic (load-balanced), allocation is
    /// O(threads), not O(chunks), and a worker that calls it keeps running
    /// chunks instead of blocking. A helper that starts after the chunks are
    /// gone does nothing. Runs inline serial for a single chunk or when
    /// already inside a parallel region (no nested fork-join). `body` must be
    /// safe to run concurrently on disjoint sub-ranges.
    template <typename Body>
        requires std::is_invocable_v<Body, std::int64_t, std::int64_t>
    dstd::Result<void> parallel_for(std::int64_t n, std::int64_t grain,
                                    Body body) {
        if (n <= 0) return {};
        const std::int64_t chunks = (n + grain - 1) / grain;
        if (chunks <= 1 || detail::in_parallel_region()) {
            body(0, n);
            return {};
        }
        struct Work {
            Body& body;
            std::int64_t n, grain;
            void run(std::int64_t, std::int64_t i, detail::ForkJoin& fj) {
                detail::ParallelRegionGuard guard;
                do {
                    body(i * grain, std::min(n, (i + 1) * grain));
                } while (fj.claim(i));
            }
        } work{body, n, grain};
        fork_join(chunks, work);
        return {};
    }

    /// Data-parallel reduction over [0, n): `map(begin, end)` produces a
    /// partial per range, combined pairwise by `combine` (associative). Each
    /// worker folds the chunks it drains into a local accumulator, then the
    /// per-worker partials are combined. `T` must be copyable (for the identity
    /// seed).
    template <typename T, typename Map, typename Combine>
    dstd::Result<T> parallel_reduce(std::int64_t n, std::int64_t grain,
                                    T identity, Map map, Combine combine) {
        if (n <= 0) return identity;
        const std::int64_t chunks = (n + grain - 1) / grain;
        if (chunks <= 1 || detail::in_parallel_region())
            return combine(std::move(identity), map(0, n));
        const std::int64_t slots = std::min<std::int64_t>(
            chunks, static_cast<std::int64_t>(threads()));
        dstd::Vector<T> partials(static_cast<std::size_t>(slots), identity);
        struct Work {
            const T& identity;
            Map& map;
            Combine& combine;
            dstd::Vector<T>& partials;
            std::int64_t n, grain;
            void run(std::int64_t slot, std::int64_t i, detail::ForkJoin& fj) {
                detail::ParallelRegionGuard guard;
                T acc = identity;
                do {
                    acc = combine(std::move(acc),
                                  map(i * grain, std::min(n, (i + 1) * grain)));
                } while (fj.claim(i));
                partials[static_cast<std::size_t>(slot)] = std::move(acc);
            }
        } work{identity, map, combine, partials, n, grain};
        fork_join(chunks, work);
        T acc = std::move(identity);
        for (T& p : partials) acc = combine(std::move(acc), std::move(p));
        return acc;
    }

    /// Wait for all outstanding tasks to complete.
    void wait_all();

    ExecutorProgress progress() const;
    bool is_responsive() const;

    /// Both fail with Pipeline when the runtime was made without a watchdog.
    dstd::Result<void> set_global_timeout(std::chrono::milliseconds timeout);
    dstd::Result<void> set_default_task_timeout(
        std::chrono::milliseconds timeout);

    /// Stops the executor. A no-op on the process runtime, which only the
    /// shutdown hooks stop.
    void shutdown();
    /// Applies `settings` in place, while work runs. Thread safe.
    void configure(const RuntimeSettings& settings);
    /// Stops the process runtime. For a shutdown hook only: nothing may
    /// submit work after it.
    static void stop_at_exit() noexcept;
    std::size_t threads() const;
    std::size_t io_threads() const;
    TaskExecutor* executor() { return executor_.get(); }
    Watchdog* watchdog() { return watchdog_.get(); }

   private:
    /// The calling thread runs chunks too and spawns at most threads() - 1
    /// helpers; it waits only for helpers that took a chunk.
    template <typename Work>
    void fork_join(std::int64_t chunks, Work& work) {
        auto state = [&] {
            memory::detail::Scope bookkeeping_scope{
                memory::detail::Manager::get().general()};
            return dstd::make_shared<detail::ForkJoin>(chunks);
        }();
        const std::int64_t helpers =
            std::min<std::int64_t>(chunks,
                                   static_cast<std::int64_t>(threads())) -
            1;
        if (!shutdown_called_.load(std::memory_order_acquire)) {
            for (std::int64_t h = 1; h <= helpers; ++h) {
                coro::detail::Coro helper =
                    detail::fork_join_helper<Work>(state, &work, h);
                helper.handle().promise().executor = executor_.get();
                executor_->post(helper.release());
            }
        }
        std::int64_t first;
        if (state->claim(first)) work.run(0, first, *state);
        if (state->in_flight.load() != 0) {
            const bool reentrant = coro::Executor::current() == executor_.get();
            coro::BlockingRegion block(reentrant ? executor_.get() : nullptr);
            state->wait_helpers();
        }
    }

    void cleanup_completed_futures();

    friend dstd::SharedPtr<Runtime> default_runtime_shared();
    friend Runtime& configure_default_runtime(const RuntimeSettings& settings);
    void become_process_runtime();
    void stop();

    dstd::UniquePtr<TaskExecutor> executor_;
    dstd::UniquePtr<Watchdog> watchdog_;
    std::atomic<std::size_t> threads_;
    runtime::io::IoBackendType io_backend_ = runtime::io::IoBackendType::Auto;
    unsigned io_batch_threshold_ = 0;
    bool process_runtime_ = false;
    std::atomic<bool> shutdown_called_{false};
    std::atomic<std::uint64_t> task_name_counter_{0};
    dstd::Vector<dstd::SharedFuture<dstd::Result<void>>> outstanding_futures_;
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
    using R = coro::TaskResult<T>;
    // The handle, its shared state and the futures list outlive the
    // submitting thread's pool scope, so they charge the general pool.
    dstd::String task_name;
    dstd::SharedPtr<dstd::Promise<dstd::SharedPtr<R>>> typed_promise;
    dstd::SharedFuture<dstd::SharedPtr<R>> typed_future;
    dstd::SharedPtr<dstd::Promise<dstd::Result<void>>> void_promise;
    dstd::SharedFuture<dstd::Result<void>> void_future;
    dstd::SharedPtr<std::atomic<common::TaskIndex>> tid;
    {
        memory::detail::Scope bookkeeping_scope{
            memory::detail::Manager::get().general()};
        if (name.empty()) {
            task_name = dstd::fmt<"task-{}">(task_name_counter_++);
        } else {
            task_name.assign(name.data(), name.size());
        }
        typed_promise = dstd::make_shared<dstd::Promise<dstd::SharedPtr<R>>>();
        typed_future = typed_promise->get_future().share();
        // void future for outstanding_futures_ tracking
        void_promise = dstd::make_shared<dstd::Promise<dstd::Result<void>>>();
        void_future = void_promise->get_future().share();
        tid = dstd::make_shared<std::atomic<common::TaskIndex>>(-1);
    }

    if (shutdown_called_.load(std::memory_order_acquire)) {
        auto refused = dstd::make_shared<R>(dstd::Unexpected<dstd::Error>(
            dstd::Error{dstd::Condition::Cancelled, "Runtime is shut down"}));
        refused->discard();
        typed_promise->set_value(std::move(refused));
        return TypedTaskHandle<T>{typed_future, -1, std::move(task_name)};
    }

    auto wrapper = [](coro::CoroTask<T> t,
                      dstd::SharedPtr<dstd::Promise<dstd::SharedPtr<R>>> tp,
                      dstd::SharedPtr<dstd::Promise<dstd::Result<void>>> vp,
                      TaskExecutor* exec,
                      dstd::SharedPtr<std::atomic<common::TaskIndex>> task_id)
        -> coro::detail::Coro {
        exec->mark_coro_started(task_id->load(std::memory_order_acquire));
        auto settled = co_await coro::settle(std::move(t));
        if (settled.error) {
            mark_coro_failed(*exec, task_id->load(std::memory_order_acquire),
                             *settled.error);
            auto failed = dstd::make_shared<R>(
                dstd::Unexpected<dstd::Error>(std::move(*settled.error)));
            failed->discard();
            tp->set_value(std::move(failed));
            vp->set_value(dstd::Result<void>{});
            co_return;
        }
        if (settled.stopped) {
            const dstd::Error error = coro::detail::stopped_error();
            mark_coro_failed(*exec, task_id->load(std::memory_order_acquire),
                             error);
            auto stopped =
                dstd::make_shared<R>(dstd::Unexpected<dstd::Error>(error));
            stopped->discard();
            tp->set_value(std::move(stopped));
            vp->set_value(dstd::Result<void>{});
            co_return;
        }
        exec->mark_coro_completed(task_id->load(std::memory_order_acquire));
        tp->set_value(dstd::make_shared<R>(std::move(*settled.value)));
        vp->set_value(dstd::Result<void>{});
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
