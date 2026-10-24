#include <dftracer/utils/dstd/no_destructor.h>
#include <dftracer/utils/runtime/coro/yield.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/executor/run_loop.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

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

namespace {

struct DriverPark {
    std::mutex mutex;
    std::condition_variable cv;
};

constinit std::atomic<std::uint32_t> g_parked_drivers{0};
// Set by the first wake after a park and cleared by the next park, so a
// burst of enqueues takes the park mutex once, not once each.
constinit std::atomic<bool> g_wake_sent{false};

// Never destroyed: a pool thread may still wake it during static teardown.
DriverPark& driver_park() noexcept {
    static NoDestructor<DriverPark> park;
    return *park;
}

}  // namespace

void notify_task_finished() noexcept {
    // Orders the caller's store of `finished` before the count read; pairs
    // with the fence in park_driver.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (g_parked_drivers.load(std::memory_order_relaxed) != 0)
        wake_parked_drivers();
}

void wake_parked_drivers() noexcept {
    if (g_parked_drivers.load(std::memory_order_acquire) == 0 ||
        g_wake_sent.exchange(true, std::memory_order_acq_rel))
        return;
    DriverPark& park = driver_park();
    {
        std::lock_guard<std::mutex> lock(park.mutex);
    }
    park.cv.notify_all();
}

bool park_driver(bool (*ready)(void*), void* arg,
                 std::chrono::milliseconds timeout) {
    DriverPark& park = driver_park();
    std::unique_lock<std::mutex> lock(park.mutex);
    g_parked_drivers.fetch_add(1, std::memory_order_relaxed);
    // Cleared on the way in, so a wake sent before this park cannot keep
    // later wakes from reaching it. The exchange also acquires what a waker
    // that skipped its notify had published.
    g_wake_sent.exchange(false, std::memory_order_acq_rel);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const bool woke = ready(arg) || park.cv.wait_for(lock, timeout) !=
                                        std::cv_status::timeout;
    g_parked_drivers.fetch_sub(1, std::memory_order_relaxed);
    return woke;
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
