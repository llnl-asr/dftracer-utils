// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_HOOKS_H
#define DFTRACER_UTILS_CORO_HOOKS_H

#include <dftracer/utils/dstd/function_ref.h>
#include <dftracer/utils/dstd/memory.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::coro {

/// How a coroutine reached an executor.
enum class CoroutineOrigin : std::uint8_t {
    Task,   ///< A task submitted to a runtime.
    Spawn,  ///< Any other post, such as a scope child or a wake.
    Io,     ///< It suspended on an I/O operation.
    Await   ///< A co_await ran it on the stack of its awaiter.
};

/**
 * @brief Callbacks that see every coroutine an executor runs.
 *
 * A callback runs on the thread of the event, in the middle of the scheduler,
 * and must not suspend, block or throw. A slow one slows that worker. To keep
 * user code off the workers, use @ref CoroutineEventLog.
 *
 * With no table installed, each event costs one relaxed load and a branch.
 */
struct CoroutineHooks {
    void* user = nullptr;
    /// A coroutine was queued. The same coroutine reports this at each wake.
    void (*enqueue)(void* user, const void* coroutine,
                    CoroutineOrigin origin) = nullptr;
    /// A worker starts a time slice of the coroutine.
    void (*resume)(void* user, const void* coroutine) = nullptr;
    /// The slice ended. The coroutine may already run on another worker or be
    /// gone, so @p coroutine is only a name here.
    void (*suspend)(void* user, const void* coroutine) = nullptr;
    /// A task reached its final suspend. It can come before the suspend of its
    /// slice, and also comes for a task that a co_await ran on its stack.
    void (*finish)(void* user, const void* coroutine) = nullptr;
    /// Optional. A co_await starts a task on the stack of its awaiter, so no
    /// queue sees it.
    void (*await_begin)(void* user, const void* coroutine) = nullptr;
    /// Optional. That task ended.
    void (*await_end)(void* user, const void* coroutine) = nullptr;
    /// Optional. The worker found no work and is about to sleep or exit.
    void (*idle)(void* user) = nullptr;
};

/**
 * @brief Installs @p hooks, or removes the table with null.
 *
 * There is one table in the process, and this replaces it. @p hooks stays valid
 * and unchanged until every executor that could read it has stopped, because a
 * worker can hold it through one time slice after the replacement. Install it
 * before the executor starts to see all of its work.
 */
void set_coroutine_hooks(const CoroutineHooks* hooks) noexcept;

namespace detail {
extern std::atomic<const CoroutineHooks*> g_coroutine_hooks;
}  // namespace detail

inline const CoroutineHooks* coroutine_hooks() noexcept {
    return detail::g_coroutine_hooks.load(std::memory_order_relaxed);
}

namespace detail {
/// For the final suspend of a promise.
inline void report_finish(const void* coroutine) noexcept {
    if (const CoroutineHooks* hooks = coroutine_hooks()) [[unlikely]] {
        hooks->finish(hooks->user, coroutine);
    }
}
}  // namespace detail

/// Index of the executor worker on this thread, -1 on any other thread. Unique
/// within one executor for the life of that executor, so two executors can both
/// have a worker 0.
int current_worker_index() noexcept;

/// Sets @ref current_worker_index for an executor's worker thread.
void set_current_worker_index(int index) noexcept;

enum class CoroutineEventKind : std::uint8_t {
    Enqueue,
    Resume,
    Suspend,
    Finish,
    AwaitBegin,
    AwaitEnd
};

struct CoroutineEvent {
    std::int64_t time_ns;    ///< Steady clock.
    const void* coroutine;   ///< Frame address. Do not dereference it.
    std::int32_t worker;     ///< @ref current_worker_index of the thread.
    CoroutineEventKind kind;
    CoroutineOrigin origin;  ///< Meaningful for Enqueue only.
};

struct CoroutineEventLogOptions {
    /// Events in one chunk. A chunk goes to the reader only when it fills or
    /// when the worker goes idle.
    std::size_t chunk_events = 4096;
    /// Chunks per worker. A worker that finds none free drops its events.
    std::size_t chunks_per_worker = 8;
};

/**
 * @brief Records coroutine events for a reader on another thread.
 *
 * A worker fills a private chunk and hands it over with one store, so the
 * reader cannot slow it and it never waits: when the reader falls behind,
 * events drop and @ref dropped counts them. Memory for a worker appears at its
 * first event. Events of one worker are in order. Order between workers comes
 * only from the time stamps.
 *
 * To see every event, stop the executors first and the log after them: a worker
 * hands over a partial chunk only when it goes idle or exits with the log
 * started, so @ref stop strands the partial chunk of a busy worker. Each worker
 * that writes keeps its chunks until the log is destroyed, so an elastic pool
 * that keeps replacing threads grows it.
 *
 * Not safe to destroy until every executor that ran while it was started has
 * stopped. At most one log runs at a time, and it replaces any other
 * @ref CoroutineHooks table.
 */
class CoroutineEventLog {
   public:
    explicit CoroutineEventLog(CoroutineEventLogOptions options = {});
    ~CoroutineEventLog();

    CoroutineEventLog(const CoroutineEventLog&) = delete;
    CoroutineEventLog& operator=(const CoroutineEventLog&) = delete;

    void start() noexcept;
    void stop() noexcept;

    /// Calls @p fn with each event handed over so far and returns how many.
    /// One reader at a time. A worker that keeps busy holds back its partial
    /// chunk.
    std::size_t drain(dstd::FunctionRef<void(const CoroutineEvent&)> fn);

    std::uint64_t dropped() const noexcept;

   private:
    struct Impl;
    dstd::UniquePtr<Impl> impl_;
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_HOOKS_H
