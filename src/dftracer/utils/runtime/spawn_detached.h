// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_SPAWN_DETACHED_H
#define DFTRACER_UTILS_RUNTIME_SPAWN_DETACHED_H

#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/task.h>

namespace dftracer::utils {

/// Runs @p task as a detached coroutine on @p executor. False, and @p task
/// never runs, when the executor is null, not running or has begun to shut
/// down. An error of the task is logged and dropped.
bool spawn_detached(coro::Executor* executor, coro::CoroTask<void> task);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_SPAWN_DETACHED_H
