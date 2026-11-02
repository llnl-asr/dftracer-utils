// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_EXECUTOR_H
#define DFTRACER_UTILS_CORO_EXECUTOR_H

#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/function_ref.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>

namespace dftracer::utils::coro {

struct ParkNode;
using ParkCancelFn = void (*)(void*) noexcept;

/// Executor - where coroutines get resumed.
///
/// @ref post is the only operation a coroutine needs, so an executor that
/// implements nothing else can still run work: its frames post themselves
/// back and the caller drives the thread. Everything else is a capability an
/// executor announces and the coro library reads before it relies on it.
class Executor {
   public:
    virtual ~Executor() = default;

    /// Queues @p h to resume on a thread of this executor. Never runs it
    /// inline and never fails.
    virtual void post(std::coroutine_handle<> h) noexcept = 0;

    /// Runs this executor's work on the calling thread until @p done holds.
    /// False when this executor cannot lend the calling thread.
    ///
    /// The caller is lending its thread rather than parking it, so an
    /// executor with a shared queue keeps serving it and loses no capacity.
    virtual bool drive_until(dstd::FunctionRef<bool()> /*done*/) {
        return false;
    }

    /// How many tasks it runs at once, a hint for fan-out limits.
    virtual std::size_t concurrency() const noexcept { return 1; }

    /// False when the executor runs only ready work on the driving thread, so
    /// a coroutine woken from another thread is not run while it waits.
    /// Awaitables that wait for such a wake-up do not wait there.
    virtual bool runs_remote_wakeups() const noexcept { return true; }

    /// Registers a waiter suspended on an awaitable that only another party can
    /// finish, so shutdown can cancel it (see ParkCancelFn). Register once the
    /// waiter is queued and before its lock is released, and unpark() the
    /// returned entry from await_resume. The default returns null and the
    /// waiter is never cancelled.
    virtual ParkNode* park(void* /*waiter*/, ParkCancelFn) noexcept {
        return nullptr;
    }
    virtual void unpark(ParkNode*) noexcept {}

    /// Timers this executor runs, or null to use @ref process_timers, the
    /// process-wide one.
    virtual TimerService* timers() noexcept { return nullptr; }

    /// Token set when the executor begins to shut down. Scopes created on
    /// this executor count as cancelled once it is set. Null when the
    /// executor never cancels.
    virtual std::shared_ptr<std::atomic<bool>> shutdown_token() const {
        return nullptr;
    }

    /// Blocking handoff around a synchronous wait made from a worker thread.
    /// enter_blocking() releases the worker's run slot and keeps the pool
    /// saturated; exit_blocking() reclaims a slot before it resumes. Paired,
    /// same thread, no-op off a worker thread. Default: no handoff.
    virtual void enter_blocking() {}
    virtual void exit_blocking() {}

    /// Executor driving the calling thread, nullptr if none.
    static Executor* current() noexcept;

    /// Sets current(), returning the previous value.
    static Executor* set_current(Executor* e) noexcept;
};

/// Binds @p exec as @ref Executor::current for the scope and restores the
/// value it replaced. Code resumed under it therefore sees the executor it
/// runs for.
class CurrentScope {
   public:
    explicit CurrentScope(Executor& exec) noexcept
        : previous_(Executor::set_current(&exec)) {}
    ~CurrentScope() { Executor::set_current(previous_); }

    CurrentScope(const CurrentScope&) = delete;
    CurrentScope& operator=(const CurrentScope&) = delete;

   private:
    Executor* previous_;
};

/// Resumes @p h under a @ref CurrentScope for @p exec.
inline void resume_on(Executor& exec, std::coroutine_handle<> h) noexcept {
    CurrentScope scope{exec};
    h.resume();
}

/// RAII blocking handoff: releases the worker's run-permit for the scope and
/// reclaims it on exit, keeping the pool saturated across a synchronous wait.
/// Default-constructed it targets the calling thread's executor; pass an
/// explicit one, or nullptr to disable. Same thread; no-op off a worker.
class BlockingRegion {
   public:
    BlockingRegion() noexcept : exec_(Executor::current()) {
        if (exec_) exec_->enter_blocking();
    }
    explicit BlockingRegion(Executor* exec) noexcept : exec_(exec) {
        if (exec_) exec_->enter_blocking();
    }
    ~BlockingRegion() {
        if (exec_) exec_->exit_blocking();
    }
    BlockingRegion(const BlockingRegion&) = delete;
    BlockingRegion& operator=(const BlockingRegion&) = delete;

   private:
    Executor* exec_;
};

/// Push a coroutine handle onto the current worker's thread-local
/// destroy list.  The worker drains this list after each resume(),
/// guaranteeing the frame is fully suspended before destruction.
void schedule_thread_local_destroy(std::coroutine_handle<> h);

/// Drain the current thread's pending-destroy list.
void drain_thread_local_destroys();

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_EXECUTOR_H
