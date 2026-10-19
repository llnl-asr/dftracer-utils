#ifndef DFTRACER_UTILS_RUNTIME_CORO_POOL_RESUME_H
#define DFTRACER_UTILS_RUNTIME_CORO_POOL_RESUME_H

#include <dftracer/utils/memory/manager.h>

#include <coroutine>

namespace dftracer::utils::coro::detail {

/// initial_suspend awaiter: the coroutine enters its pool when it first runs.
struct EnterPoolOnStart {
    memory::PoolTag pool;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept { memory::enter_pool(pool); }
};

/// Wraps an awaitable so the coroutine enters its pool again when it resumes
/// after a suspension, whichever thread resumes it.
template <typename Inner>
struct InPool {
    Inner inner_;
    memory::PoolTag pool_;
    bool suspended_ = false;

    bool await_ready() noexcept(noexcept(inner_.await_ready())) {
        return inner_.await_ready();
    }

    template <typename Handle>
    auto await_suspend(Handle h) noexcept(noexcept(inner_.await_suspend(h))) {
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
