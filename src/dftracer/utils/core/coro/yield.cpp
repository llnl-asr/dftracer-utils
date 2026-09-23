#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/run_loop.h>

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
    if (!Executor::current()) {
        return false;  // Not on a worker thread
    }
    auto elapsed = Clock::now() - tls_timeslice_start;
    return elapsed >=
           std::chrono::duration_cast<Clock::duration>(tls_timeslice_duration);
}

void set_timeslice_duration(std::chrono::microseconds duration) noexcept {
    tls_timeslice_duration = duration;
}

std::chrono::microseconds get_timeslice_duration() noexcept {
    return tls_timeslice_duration;
}

void* suppress_executor() noexcept {
    auto* old = Executor::set_current(nullptr);
    return static_cast<void*>(old);
}

void restore_executor(void* saved) noexcept {
    Executor::set_current(static_cast<Executor*>(saved));
}

void drive_to_completion(std::coroutine_handle<> h,
                         const std::atomic<bool>& finished) {
    if (!h) return;
    auto is_finished = [&finished] {
        return finished.load(std::memory_order_acquire);
    };

    auto saved = get_timeslice_duration();
    set_timeslice_duration(std::chrono::microseconds{0});
    struct RestoreTimeslice {
        std::chrono::microseconds saved;
        ~RestoreTimeslice() { set_timeslice_duration(saved); }
    } restore{saved};

    // Lend this thread to the executor already driving it rather than
    // standing up a private loop: a pool keeps serving its queue meanwhile,
    // so waiting here costs it no capacity and work this task depends on can
    // still run - including on this thread.
    if (auto* exec = Executor::current()) {
        exec->enqueue(h);
        exec->drive_until(is_finished);
        return;
    }

    RunLoop loop;
    struct Bind {
        Executor* prev;
        ~Bind() { Executor::set_current(prev); }
    } bind{Executor::set_current(&loop)};

    loop.enqueue(h);
    loop.drive_until(is_finished);
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
