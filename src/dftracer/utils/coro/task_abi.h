// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_TASK_ABI_H
#define DFTRACER_UTILS_CORO_TASK_ABI_H

#include <dftracer/utils/coro/abi.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>

namespace dftracer::utils {

/// Take ownership of `task` and return it as an opaque dftu_task, freed when a
/// driver (dftu_task_run/when_all/when_any) runs it.
dftu_task* task_to_abi(coro::CoroTask<void>&& task);

/// Take back a task that @ref task_to_abi made, to await or destroy it.
dstd::UniquePtr<coro::CoroTask<void>> task_from_abi(dftu_task* task) noexcept;

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORO_TASK_ABI_H
