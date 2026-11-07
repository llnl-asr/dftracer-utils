// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_RUN_ON_H
#define DFTRACER_UTILS_CORO_RUN_ON_H

#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/task.h>

#include <coroutine>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

namespace detail {

/// Moves the awaiting task onto `target`. The task's executor becomes
/// `target`, so the children it awaits run there too.
struct SwitchTo {
    Executor* target;
    bool await_ready() const noexcept { return false; }
    template <class Promise>
    void await_suspend(std::coroutine_handle<Promise> h) const noexcept {
        if constexpr (std::is_base_of_v<PromiseBase, Promise>) {
            h.promise().set_executor(target);
        }
        target->post(h);
    }
    void await_resume() const noexcept {}
};

}  // namespace detail

/// Runs `task` on `ex`. The caller resumes on its own executor afterwards.
template <class T>
CoroTask<T> run_on(Executor& ex, CoroTask<T> task) {
    detail::SwitchTo hop{&ex};
    co_await hop;
    if constexpr (std::is_void_v<T>) {
        co_await std::move(task);
    } else {
        co_return co_await std::move(task);
    }
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_RUN_ON_H
