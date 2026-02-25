#ifndef DFTRACER_UTILS_CORE_CORO_YIELD_H
#define DFTRACER_UTILS_CORE_CORO_YIELD_H

#include <chrono>
#include <coroutine>
#include <type_traits>

namespace dftracer::utils {
class Executor;
}  // namespace dftracer::utils

namespace dftracer::utils::coro {

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
/// Returns the previous value.  Used by SyncScope.
void* suppress_executor() noexcept;
void restore_executor(void* saved) noexcept;

/// RAII guard that suppresses the executor context and timeslice
/// for the current scope, forcing nested async I/O into the
/// synchronous pread/pwrite fallback.  Used by CoroTask::get().
struct SyncScope {
    void* saved_exec_;
    std::chrono::microseconds saved_ts_;

    SyncScope() noexcept
        : saved_exec_(suppress_executor()),
          saved_ts_(get_timeslice_duration()) {
        set_timeslice_duration(std::chrono::microseconds{0});
    }
    ~SyncScope() noexcept {
        set_timeslice_duration(saved_ts_);
        restore_executor(saved_exec_);
    }
    SyncScope(const SyncScope&) = delete;
    SyncScope& operator=(const SyncScope&) = delete;
};

/// Re-enqueue a coroutine handle on the current worker's executor.
/// No-op if not on a worker thread.  Resets the timeslice.
void yield_to_executor(std::coroutine_handle<> h) noexcept;

// ============================================================================
// yield() and maybe_yield() awaitables
// ============================================================================

/// Tag type so await_transform can recognize and skip wrapping.
struct YieldAwaitable {
    bool force_ =
        false;  // true = always suspend, false = only if timeslice exceeded

    bool await_ready() noexcept;

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept;

    void await_resume() noexcept;
};

/// Unconditional yield -- always suspends and re-enqueues on the executor.
inline YieldAwaitable yield() noexcept { return {true}; }

/// Conditional yield -- only suspends if the current thread's timeslice
/// has been exceeded.  No-op (~25ns clock read) in the common case.
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
template <typename Inner>
struct YieldCheckAwaitable {
    Inner inner_;
    bool yielding_ = false;

    bool await_ready() noexcept(noexcept(inner_.await_ready())) {
        bool inner_ready = inner_.await_ready();
        if (inner_ready && timeslice_exceeded()) {
            yielding_ = true;
            return false;  // Force suspend to yield
        }
        return inner_ready;
    }

    template <typename Handle>
    std::coroutine_handle<> await_suspend(Handle h) noexcept(
        noexcept(inner_.await_suspend(h))) {
        if (yielding_) {
            yield_to_executor(h);
            return std::noop_coroutine();
        }
        // Delegate to inner awaitable.
        // Handle all three await_suspend return types.
        using R = decltype(inner_.await_suspend(h));
        if constexpr (std::is_void_v<R>) {
            inner_.await_suspend(h);
            return std::noop_coroutine();
        } else if constexpr (std::is_same_v<R, bool>) {
            if (inner_.await_suspend(h)) {
                return std::noop_coroutine();
            }
            return h;  // Resume immediately
        } else {
            return inner_.await_suspend(h);
        }
    }

    auto await_resume() noexcept(noexcept(inner_.await_resume()))
        -> decltype(inner_.await_resume()) {
        return inner_.await_resume();
    }
};

}  // namespace detail

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_YIELD_H
