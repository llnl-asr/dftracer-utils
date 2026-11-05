// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_DETAIL_RESUMPTION_HELPER_H
#define DFTRACER_UTILS_CORO_DETAIL_RESUMPTION_HELPER_H

#include <coroutine>

namespace dftracer::utils::coro {
class Executor;
}

namespace dftracer::utils::coro::detail {

/// Posts `handle` to `executor`, or to whichever executor drives this thread
/// when it names none. With neither it logs and aborts, since every task
/// carries an executor and a dropped resume would hang far from the cause.
void post_resumption(Executor* executor, std::coroutine_handle<> handle);

/// Executor a coroutine suspending right now must be resumed through:
/// `preferred` when it has one, else whichever is driving this thread.
/// Parking a waiter with neither means it can never be woken.
Executor* resume_executor_for(Executor* preferred) noexcept;

/// Marks a suspending coroutine and its root as waiting on another party, so
/// the executor does not resume them itself.
template <typename Promise>
void set_awaiting_async(std::coroutine_handle<Promise> h, bool value) {
    if constexpr (requires(Promise& p) { p.awaiting_async = value; }) {
        h.promise().awaiting_async = value;
    }
    if constexpr (requires(Promise& p) {
                      p.get_root_promise();
                      p.get_root_promise()->awaiting_async = value;
                  }) {
        if (auto* root = h.promise().get_root_promise()) {
            root->awaiting_async = value;
        }
    }
}

template <typename Promise>
Executor* executor_for(std::coroutine_handle<Promise> h) {
    if constexpr (requires(Promise& p) {
                      p.get_root_promise();
                      p.get_root_promise()->get_executor();
                  }) {
        auto* root = h.promise().get_root_promise();
        return resume_executor_for(root ? root->get_executor() : nullptr);
    } else {
        return resume_executor_for(nullptr);
    }
}

}  // namespace dftracer::utils::coro::detail

#endif  // DFTRACER_UTILS_CORO_DETAIL_RESUMPTION_HELPER_H
