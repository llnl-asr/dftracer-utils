#ifndef DFTRACER_UTILS_RUNTIME_EXECUTOR_EXECUTOR_H
#define DFTRACER_UTILS_RUNTIME_EXECUTOR_EXECUTOR_H

#include <dftracer/utils/common/typedefs.h>
#include <dftracer/utils/runtime/executor/timer_service.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <memory>

namespace dftracer::utils {

struct ParkNode;
using ParkCancelFn = void (*)(void*) noexcept;

namespace io {
class IoBackend;
}  // namespace io

/// Executor - where coroutines get resumed.
class Executor {
   public:
    virtual ~Executor() = default;

    virtual void enqueue(std::coroutine_handle<> handle,
                         TaskIndex task_id = -1) = 0;
    virtual void schedule_coroutine_resumption(
        std::coroutine_handle<> handle) = 0;
    virtual void schedule_destroy(std::coroutine_handle<> handle) = 0;
    virtual void mark_coro_completed(TaskIndex id) = 0;

    virtual bool is_running() const = 0;
    virtual std::size_t get_num_threads() const = 0;
    virtual std::size_t get_io_pool_size() const = 0;
    virtual TimerService& get_timer_service() = 0;

    /// Run pending work on the calling thread until `done()` holds.
    ///
    /// The caller is lending its thread rather than parking it, so an
    /// executor with a shared queue keeps serving it and loses no capacity.
    virtual void drive_until(const std::function<bool()>& done) = 0;

    /// False when the executor runs only work that is ready on the driving
    /// thread, so a coroutine woken from another thread (a timer, a release)
    /// is never run while it waits. Awaitables that wait for such a wake-up
    /// do not wait there.
    virtual bool runs_remote_wakeups() const noexcept { return true; }

    /// Registers a waiter suspended on an awaitable that only another party
    /// can finish, so shutdown can call `cancel(waiter)` (see ParkCancelFn).
    /// Register once the waiter is queued and before its lock is released,
    /// and unpark() the returned entry from await_resume. Default: no
    /// registry, returns null and the waiter is never cancelled.
    virtual ParkNode* park(void* /*waiter*/, ParkCancelFn) noexcept {
        return nullptr;
    }
    virtual void unpark(ParkNode*) noexcept {}

    /// Token set when the executor begins to shut down. Scopes created on
    /// this executor count as cancelled once it is set. Null when the
    /// executor never cancels.
    virtual std::shared_ptr<std::atomic<bool>> shutdown_token() const {
        return nullptr;
    }

    /// Without a backend the io layer falls back to blocking syscalls.
    virtual bool has_io_backend() const noexcept = 0;
    virtual io::IoBackend& io_backend() = 0;

    /// Executor driving the calling thread, nullptr if none.
    static Executor* current() noexcept;

    /// Sets current(), returning the previous value.
    static Executor* set_current(Executor* e) noexcept;

    /// Blocking handoff around a synchronous wait made from a worker thread.
    /// enter_blocking() releases the worker's run slot and keeps the pool
    /// saturated; exit_blocking() reclaims a slot before it resumes. Paired,
    /// same thread, no-op off a worker thread. Default: no handoff.
    virtual void enter_blocking() {}
    virtual void exit_blocking() {}
};

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

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_EXECUTOR_EXECUTOR_H
