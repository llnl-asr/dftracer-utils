#ifndef DFTRACER_UTILS_CORE_EXECUTOR_RESUMPTION_HELPER_H
#define DFTRACER_UTILS_CORE_EXECUTOR_RESUMPTION_HELPER_H

#include <coroutine>

namespace dftracer::utils {

class Executor;

void schedule_coroutine_resumption_helper(Executor* executor,
                                          std::coroutine_handle<> handle);

/// Executor a coroutine suspending right now must be resumed through:
/// `preferred` when it has one, else whichever is driving this thread.
/// Parking a waiter with neither means it can never be woken.
Executor* resume_executor_for(Executor* preferred) noexcept;

/// Schedule a coroutine handle for deferred destruction.
/// Used by SpawnFuture::detach() when the waiter handle is extracted
/// but will never be resumed (preventing frame leaks).
void schedule_destroy_helper(Executor* executor,
                             std::coroutine_handle<> handle);
}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_EXECUTOR_RESUMPTION_HELPER_H
