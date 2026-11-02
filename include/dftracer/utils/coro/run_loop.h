// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_RUN_LOOP_H
#define DFTRACER_UTILS_CORO_RUN_LOOP_H

#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/parked.h>
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/deque.h>
#include <dftracer/utils/dstd/function_ref.h>

#include <condition_variable>
#include <coroutine>
#include <mutex>

namespace dftracer::utils::coro {

/**
 * RunLoop - an @ref Executor that runs coroutines on the calling thread.
 *
 * Owns no threads: @ref post queues a handle and @ref drive_until resumes
 * queued handles in turn until the predicate holds, so concurrency comes
 * without parallelism. This is the loop a caller with no runtime gets, and the
 * building block for an executor that is driven rather than run by a pool.
 *
 * One thread drives it at a time; any thread may post to it meanwhile. Because
 * a driver is the only thread that resumes work, @ref runs_remote_wakeups is
 * false, and an awaitable that would wait for a wake-up from another thread
 * takes another path. The io layer finds no I/O backend under this loop, so
 * `io::` calls take their blocking-syscall path rather than waiting on
 * completions nothing would poll.
 */
class RunLoop final : public Executor {
   public:
    RunLoop() = default;
    ~RunLoop() override { timer_service_.stop(); }

    RunLoop(const RunLoop&) = delete;
    RunLoop& operator=(const RunLoop&) = delete;

    /// Queues @p handle for the driving thread to resume. Safe from any
    /// thread.
    void post(std::coroutine_handle<> handle) noexcept override;

    /**
     * Resumes ready coroutines until `done()`, then runs whatever the finished
     * work left runnable until the queue is empty. Always lends the calling
     * thread, so it returns true.
     *
     * With the queue empty and `done()` false, waits while a parked waiter
     * (see @ref park) exists, since another thread can still wake it. Throws
     * when there is none: this loop is the only thing that could resume the
     * awaited work, so waiting longer would hang forever.
     */
    bool drive_until(dstd::FunctionRef<bool()> done) override;

    bool runs_remote_wakeups() const noexcept override { return false; }

    /// The loop's own timers, started on the first call from any thread and
    /// stopped when the loop is destroyed.
    TimerService* timers() noexcept override {
        timer_service_.start();
        return &timer_service_;
    }

    ParkNode* park(void* waiter, ParkCancelFn cancel) noexcept override {
        return parked_.park(waiter, cancel);
    }
    void unpark(ParkNode* n) noexcept override { parked_.unpark(n); }

   private:
    bool run_one();
    [[noreturn]] void throw_stalled();

    std::mutex mutex_;
    std::condition_variable ready_cv_;
    ParkRegistry parked_;
    dstd::Deque<std::coroutine_handle<>> ready_;
    TimerService timer_service_;
};

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_RUN_LOOP_H
