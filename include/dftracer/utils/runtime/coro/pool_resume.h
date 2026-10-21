#ifndef DFTRACER_UTILS_RUNTIME_CORO_POOL_RESUME_H
#define DFTRACER_UTILS_RUNTIME_CORO_POOL_RESUME_H

#include <dftracer/utils/common/intern_table.h>
#include <dftracer/utils/memory/manager.h>

#include <coroutine>
#include <type_traits>

namespace dftracer::utils::coro::detail {

/// What a coroutine runs in: the pool and the intern table that were current
/// when it was created. The frame holds a reference to the table, taken only
/// when there is one, so the common case costs no atomic.
struct RunContext {
    memory::TaskTag tag{memory::current_task()};

    RunContext() noexcept {
        if (auto* table = intern()) table->add_ref();
    }
    RunContext(const RunContext&) = delete;
    RunContext& operator=(const RunContext&) = delete;
    ~RunContext() {
        if (auto* table = intern()) table->release();
    }

    InternTable* intern() const noexcept {
        return static_cast<InternTable*>(tag.user);
    }

    void enter() const noexcept { memory::enter_task(tag); }
};

/// initial_suspend awaiter: the coroutine enters its context when it first
/// runs.
struct EnterPoolOnStart {
    const RunContext* context;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept { context->enter(); }
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

/// Wraps an awaitable so the coroutine enters its context again when it
/// resumes after a suspension, whichever thread resumes it. A wrapper made with
/// `cancelled_` set ends the task as cancelled at this await instead.
template <typename Inner>
struct InPool {
    Inner inner_;
    const RunContext* context_;
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
        if (suspended_) context_->enter();
        return inner_.await_resume();
    }
};

/// The await_transform of a promise that keeps its context in `context`. The
/// operand is used in place, never moved: a caller may still act on it after
/// the co_await (when_any detaches its losers), and a temporary operand lives
/// until the end of the full expression, across the suspension.
template <typename U>
InPool<U&&> in_pool(U&& awaitable, const RunContext& context) noexcept {
    return InPool<U&&>{static_cast<U&&>(awaitable), &context};
}

}  // namespace dftracer::utils::coro::detail

#endif  // DFTRACER_UTILS_RUNTIME_CORO_POOL_RESUME_H
