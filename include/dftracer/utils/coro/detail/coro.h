// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_DETAIL_CORO_H
#define DFTRACER_UTILS_CORO_DETAIL_CORO_H

#include <dftracer/utils/common/typedefs.h>
#include <dftracer/utils/coro/detail/frame.h>
#include <dftracer/utils/coro/detail/pool_resume.h>
#include <dftracer/utils/memory/detail/manager.h>

#include <atomic>
#include <coroutine>
#include <exception>
#include <utility>

namespace dftracer::utils::coro {
class Executor;
}

namespace dftracer::utils::coro::detail {

struct CoroPromise;

/// Lightweight fire-and-forget coroutine type.
///
/// Unlike CoroTask<T>, Coro has no return value (communicate
/// through channels), no continuation chain (flat scheduling), and
/// integrates with @ref JoinHandle for structured concurrency.
///
/// Coro is the internal execution primitive. Users interact with
/// CoroTask and @ref Scope; internally, each spawned task is wrapped in a Coro
/// and enqueued to the Executor.
class Coro {
   public:
    using promise_type = CoroPromise;

    explicit Coro(std::coroutine_handle<CoroPromise> h) : handle_(h) {}

    ~Coro() {
        if (handle_) {
            auto h = handle_;
            handle_ = nullptr;
            h.destroy();
        }
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
    /// FinalAwaiter hands a released frame to the thread-local destroy
    /// list, drained once the resume that finished it returned.
    /// Used when handing the handle to the Executor's run queue.
    inline std::coroutine_handle<CoroPromise> release();

   private:
    std::coroutine_handle<CoroPromise> handle_;
};

/// What a task the runtime tracks reports to, filled in when that task is
/// spawned. @ref CoroPromise calls it once at the task's final suspend, and a
/// promise with no tracker reports nothing.
struct TaskTracking {
    void (*on_complete)(void* owner, common::TaskIndex id) noexcept;
    void* owner;
    common::TaskIndex id;
};

struct CoroPromise : FrameAllocation {
    std::exception_ptr exception{nullptr};

    /// Join group: points to @ref JoinHandle's atomic counter.
    /// FinalAwaiter decrements on completion.
    std::atomic<std::size_t>* join_counter{nullptr};

    /// Continuation to resume when join group reaches zero.
    /// Points to @ref JoinHandle's atomic<void*>.
    std::atomic<void*>* join_continuation{nullptr};

    /// Executor for global scheduling (set before enqueue).
    Executor* executor{nullptr};

    /// Tracker of this task, null when it is untracked (set before enqueue).
    TaskTracking* tracking{nullptr};

    detail::RunContext context;

    /// True when Coro::release() transferred ownership to the queue.
    /// FinalAwaiter uses this to schedule deferred destruction.
    bool released{false};

    /// Set by the creator of a frame that may end at a cancelled wait, as a
    /// pump of a stream does. Any other frame has no way to finish early, and a
    /// cancel that reaches its wait ends the process.
    bool stoppable{false};

    bool refuses_stop() const noexcept { return !stoppable; }
    bool cancel_requested() const noexcept { return false; }

    /// Ends the frame at its current wait as if it had returned, so the wait
    /// never resumes and the locals die with the frame.
    void finish_cancelled() noexcept;

    Coro get_return_object() {
        return Coro{std::coroutine_handle<CoroPromise>::from_promise(*this)};
    }

    detail::EnterPoolOnStart initial_suspend() noexcept { return {&context}; }

    template <typename U>
    auto await_transform(U&& awaitable) noexcept {
        return detail::in_pool(std::forward<U>(awaitable), context);
    }

    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }

        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<CoroPromise> h) noexcept;

        /// What the final suspend does, and the join waiter to resume.
        static std::coroutine_handle<> finish(
            std::coroutine_handle<CoroPromise> h) noexcept;

        void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    /// Told of an exception that ends the frame, in place of holding it. Set
    /// by whoever reports the failures of the frame, such as a @ref Scope.
    void (*exception_sink)(void* owner,
                           std::exception_ptr error) noexcept {nullptr};
    void* sink_owner{nullptr};

    void unhandled_exception() {
        exception = std::current_exception();
        if (exception_sink != nullptr) {
            exception_sink(sink_owner, std::move(exception));
            exception = nullptr;
        }
    }
};

/// Out-of-line: needs @ref CoroPromise to be complete.
inline std::coroutine_handle<CoroPromise> Coro::release() {
    auto h = handle_;
    h.promise().released = true;
    handle_ = nullptr;
    return h;
}

}  // namespace dftracer::utils::coro::detail

#endif  // DFTRACER_UTILS_CORO_DETAIL_CORO_H
