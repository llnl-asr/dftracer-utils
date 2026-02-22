#ifndef DFTRACER_UTILS_CORE_CORO_CORO_H
#define DFTRACER_UTILS_CORE_CORO_CORO_H

#include <atomic>
#include <coroutine>
#include <exception>

namespace dftracer::utils {
class Executor;
}  // namespace dftracer::utils

namespace dftracer::utils::coro {

struct CoroPromise;

/// Lightweight fire-and-forget coroutine type.
///
/// Unlike CoroTask<T>, Coro has no return value (communicate
/// through channels), no continuation chain (flat scheduling), and
/// integrates with JoinHandle for structured concurrency.
///
/// Coro is the internal execution primitive. Users interact
/// with Task (the DAG API); internally, each ready Task is wrapped
/// in a run_task() coroutine and enqueued to the Executor.
class Coro {
   public:
    using promise_type = CoroPromise;

    explicit Coro(std::coroutine_handle<CoroPromise> h) : handle_(h) {}

    ~Coro() {
        if (handle_) handle_.destroy();
    }

    Coro(Coro&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }

    Coro& operator=(Coro&& o) noexcept {
        if (this != &o) {
            if (handle_) handle_.destroy();
            handle_ = o.handle_;
            o.handle_ = nullptr;
        }
        return *this;
    }

    Coro(const Coro&) = delete;
    Coro& operator=(const Coro&) = delete;

    std::coroutine_handle<CoroPromise> handle() const { return handle_; }
    bool done() const { return handle_ && handle_.done(); }

    /// Release ownership of the coroutine handle.
    /// After this call, the Coro no longer owns the handle.
    /// FinalAwaiter will schedule deferred destruction for
    /// released handles (via Executor::schedule_destroy).
    /// Used when handing the handle to the Executor's run queue.
    inline std::coroutine_handle<CoroPromise> release();

   private:
    std::coroutine_handle<CoroPromise> handle_;
};

struct CoroPromise {
    std::exception_ptr exception{nullptr};

    /// Join group: points to JoinHandle's atomic counter.
    /// FinalAwaiter decrements on completion.
    std::atomic<std::size_t>* join_counter{nullptr};

    /// Continuation to resume when join group reaches zero.
    /// Points to JoinHandle's atomic<void*>.
    std::atomic<void*>* join_continuation{nullptr};

    /// Executor for global scheduling (set before enqueue).
    Executor* executor{nullptr};

    /// True when Coro::release() transferred ownership to the queue.
    /// FinalAwaiter uses this to schedule deferred destruction.
    bool released{false};

    Coro get_return_object() {
        return Coro{std::coroutine_handle<CoroPromise>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }

        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<CoroPromise> h) noexcept;

        void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() { exception = std::current_exception(); }
};

// Out-of-line: needs CoroPromise to be complete.
inline std::coroutine_handle<CoroPromise> Coro::release() {
    auto h = handle_;
    h.promise().released = true;
    handle_ = nullptr;
    return h;
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_CORO_H
