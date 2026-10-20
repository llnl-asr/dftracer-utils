#ifndef DFTRACER_UTILS_RUNTIME_CORO_POOL_RESUME_H
#define DFTRACER_UTILS_RUNTIME_CORO_POOL_RESUME_H

#include <dftracer/utils/memory/manager.h>

#include <coroutine>
#include <type_traits>

namespace dftracer::utils::coro::detail {

/// initial_suspend awaiter: the coroutine enters its pool when it first runs.
struct EnterPoolOnStart {
    memory::PoolTag pool;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept { memory::enter_pool(pool); }
};

/// Ends the task of `h` as cancelled at its current await, in place of
/// suspending on the awaitable, and returns what an await_suspend of type R
/// returns to stay suspended. The frame is not resumed again: its owner
/// destroys it. See PromiseBase::finish_cancelled.
template <typename R, typename Handle>
R end_cancelled(Handle h) noexcept {
    h.promise().finish_cancelled();
    if constexpr (std::is_void_v<R>) {
        return;
    } else if constexpr (std::is_same_v<R, bool>) {
        return true;
    } else {
        return R{std::noop_coroutine()};
    }
}

/// Wraps an awaitable so the coroutine enters its pool again when it resumes
/// after a suspension, whichever thread resumes it. A wrapper made with
/// `cancelled_` set ends the task as cancelled at this await instead.
template <typename Inner>
struct InPool {
    Inner inner_;
    memory::PoolTag pool_;
    bool suspended_ = false;
    bool cancelled_ = false;

    bool await_ready() noexcept(noexcept(inner_.await_ready())) {
        return !cancelled_ && inner_.await_ready();
    }

    template <typename Handle>
    auto await_suspend(Handle h) noexcept(noexcept(inner_.await_suspend(h))) {
        using R = decltype(inner_.await_suspend(h));
        if constexpr (requires { h.promise().finish_cancelled(); }) {
            if (cancelled_) return end_cancelled<R>(h);
        }
        suspended_ = true;
        return inner_.await_suspend(h);
    }

    auto await_resume() noexcept(noexcept(inner_.await_resume()))
        -> decltype(inner_.await_resume()) {
        if (suspended_) memory::enter_pool(pool_);
        return inner_.await_resume();
    }
};

/// The await_transform of a promise that keeps its pool in `pool`. The operand
/// is used in place, never moved: a caller may still act on it after the
/// co_await (when_any detaches its losers), and a temporary operand lives
/// until the end of the full expression, across the suspension.
template <typename U>
InPool<U&&> in_pool(U&& awaitable, memory::PoolTag pool) noexcept {
    return InPool<U&&>{static_cast<U&&>(awaitable), pool};
}

}  // namespace dftracer::utils::coro::detail

#endif  // DFTRACER_UTILS_RUNTIME_CORO_POOL_RESUME_H
