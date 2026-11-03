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

}  // namespace dftracer::utils::coro::detail

#endif  // DFTRACER_UTILS_CORO_DETAIL_RESUMPTION_HELPER_H
