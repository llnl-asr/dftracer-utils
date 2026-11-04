// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_YIELD_H
#define DFTRACER_UTILS_CORO_YIELD_H

#include <dftracer/utils/coro/detail/pool_resume.h>
#include <dftracer/utils/memory/detail/manager.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <type_traits>

namespace dftracer::utils::coro {

class Executor;

// ============================================================================
// Thread-local timeslice state (set by worker loop)
// ============================================================================

/// Default timeslice duration (10ms).
inline constexpr std::chrono::microseconds DEFAULT_TIMESLICE{10'000};

/// Reset the current thread's timeslice clock to now.
void reset_timeslice() noexcept;

/// Check whether the current thread has exceeded its timeslice.
bool timeslice_exceeded() noexcept;

/// Set the timeslice duration for the current thread.
void set_timeslice_duration(std::chrono::microseconds duration) noexcept;

/// Get the timeslice duration for the current thread.
std::chrono::microseconds get_timeslice_duration() noexcept;

/// Suppress (or restore) the current-thread executor context.
/// Returns the previous value.  Used by @ref SyncScope.
void* suppress_executor() noexcept;
void restore_executor(void* saved) noexcept;

/// RAII guard that suppresses the executor context and timeslice
/// for the current scope, forcing nested async I/O into the
/// synchronous pread/pwrite fallback.  Used by CoroTask::get().
struct SyncScope {
    void* saved_exec;
    std::chrono::microseconds saved_ts;

    SyncScope() noexcept
        : saved_exec(suppress_executor()), saved_ts(get_timeslice_duration()) {
        set_timeslice_duration(std::chrono::microseconds{0});
    }
    ~SyncScope() noexcept {
        set_timeslice_duration(saved_ts);
        restore_executor(saved_exec);
    }
    SyncScope(const SyncScope&) = delete;
    SyncScope& operator=(const SyncScope&) = delete;
};

/// Re-enqueue a coroutine handle on the current worker's executor.
/// No-op if not on a worker thread.  Resets the timeslice.
void yield_to_executor(std::coroutine_handle<> h) noexcept;

/// Run `h` until it sets `finished` (a release store at its final suspend)
/// on the calling thread. Waiting on that flag, not on h.done(), keeps the
/// wait race-free while another worker resumes `h`, and makes its result
/// visible to this thread.
///
/// With no executor bound to this thread, work spawned by `h` would have
/// nothing to resume it, so a RunLoop is installed for the duration.
void drive_to_completion(std::coroutine_handle<> h,
                         const std::atomic<bool>& finished);

/// Wakes the threads parked in an executor's drive_until after a task with
/// no awaiter ended, so a blocked get() sees it without polling. Call it
/// after the release store of `finished`. Cheap when nobody is parked.
void notify_task_finished() noexcept;

/// Wakes the threads parked in drive_until, for new work or a shutdown.
void wake_parked_drivers() noexcept;

/// Parks a thread in drive_until until `ready(arg)` holds, a task with no
/// awaiter ends, wake_parked_drivers() is called or `timeout` passes. `ready`
/// is checked after the thread counts as parked, so a wake-up that races
/// with the park is not lost. False when the timeout passed.
bool park_driver(bool (*ready)(void*), void* arg,
                 std::chrono::milliseconds timeout);

// ============================================================================
// yield() and maybe_yield() awaitables
// ============================================================================

/// Tag type so await_transform can recognize and skip wrapping.
struct YieldAwaitable {
    bool force =
        false;  ///< true = always suspend, false = only if timeslice exceeded

    bool await_ready() noexcept;

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept;

    void await_resume() noexcept;
};

/// Unconditional yield -- always suspends and re-enqueues on the executor.
inline YieldAwaitable yield() noexcept { return {true}; }

/// Conditional yield -- only suspends if the current thread's timeslice
/// has been exceeded.  No-op (one clock read) in the common case.
///
/// Usage:
///   for (auto& item : large_dataset) {
///       process(item);
///       co_await maybe_yield();
///   }
inline YieldAwaitable maybe_yield() noexcept { return {false}; }

// ============================================================================
// YieldCheckAwaitable -- wraps any awaitable with timeslice check
// ============================================================================

namespace detail {

/// Wraps an inner awaitable.  If the inner is already ready AND the
/// timeslice is exceeded, forces a yield before continuing.
/// When the inner is NOT ready (coroutine will suspend anyway),
/// passes through with zero overhead -- no clock read.
/// After a suspension the coroutine enters its context again.
template <typename Inner>
struct YieldCheckAwaitable {
    Inner inner;
    const RunContext* context;
    bool yielding = false;
    bool suspended = false;
    /// The task was cancelled: it ends here, as cancelled, and the inner
    /// awaitable is never touched.
    bool cancelled = false;

    bool await_ready() noexcept(noexcept(inner.await_ready())) {
        if (cancelled) return false;
        bool inner_ready = inner.await_ready();
        if (inner_ready && timeslice_exceeded()) {
            yielding = true;
            return false;  // Force suspend to yield
        }
        return inner_ready;
    }

    // Keeps the inner return type: turning a bool into a handle back to h
    // would be a symmetric transfer, which grows the stack without
    // sibling-call optimization (see CoroTask::await_suspend).
    template <typename Handle>
    auto await_suspend(Handle h) noexcept(noexcept(inner.await_suspend(h))) {
        using R = decltype(inner.await_suspend(h));
        if constexpr (requires { h.promise().finish_cancelled(); }) {
            if (cancelled) {
                h.promise().finish_cancelled();
                if constexpr (std::is_void_v<R> || std::is_same_v<R, bool>) {
                    return true;
                } else {
                    return std::coroutine_handle<>{std::noop_coroutine()};
                }
            }
        }
        suspended = true;
        if constexpr (std::is_void_v<R> || std::is_same_v<R, bool>) {
            if (yielding) {
                yield_to_executor(h);
                return true;
            }
            if constexpr (std::is_void_v<R>) {
                inner.await_suspend(h);
                return true;
            } else {
                return inner.await_suspend(h);
            }
        } else {
            if (yielding) {
                yield_to_executor(h);
                return std::coroutine_handle<>{std::noop_coroutine()};
            }
            return std::coroutine_handle<>{inner.await_suspend(h)};
        }
    }

    auto await_resume() noexcept(noexcept(inner.await_resume()))
        -> decltype(inner.await_resume()) {
        if (suspended) context->enter();
        return inner.await_resume();
    }
};

}  // namespace detail

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_YIELD_H
