#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/pipeline/executor.h>

#include <chrono>

namespace dftracer::utils::coro {

using Clock = std::chrono::steady_clock;

static thread_local Clock::time_point tls_timeslice_start = Clock::time_point{};
static thread_local std::chrono::microseconds tls_timeslice_duration =
    DEFAULT_TIMESLICE;

void reset_timeslice() noexcept { tls_timeslice_start = Clock::now(); }

bool timeslice_exceeded() noexcept {
    if (tls_timeslice_duration.count() == 0) {
        return false;  // Yielding disabled
    }
    auto elapsed = Clock::now() - tls_timeslice_start;
    return elapsed >=
           std::chrono::duration_cast<Clock::duration>(tls_timeslice_duration);
}

void set_timeslice_duration(std::chrono::microseconds duration) noexcept {
    tls_timeslice_duration = duration;
}

void yield_to_executor(std::coroutine_handle<> h) noexcept {
    auto* exec = Executor::current();
    if (exec) {
        reset_timeslice();
        exec->enqueue(h);
    }
}

// ============================================================================
// YieldAwaitable implementation
// ============================================================================

bool YieldAwaitable::await_ready() noexcept {
    if (force_) return false;  // Unconditional yield
    return !timeslice_exceeded();
}

std::coroutine_handle<> YieldAwaitable::await_suspend(
    std::coroutine_handle<> h) noexcept {
    auto* exec = Executor::current();
    if (exec) {
        reset_timeslice();
        exec->enqueue(h);
        return std::noop_coroutine();
    }
    // Not on a worker thread.
    // force_ (yield): park the coroutine -- caller drives resume manually.
    // !force_ (maybe_yield): no executor to enqueue on -- resume inline.
    return force_ ? std::noop_coroutine() : h;
}

void YieldAwaitable::await_resume() noexcept {}

}  // namespace dftracer::utils::coro
