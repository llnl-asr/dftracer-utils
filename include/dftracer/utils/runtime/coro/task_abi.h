#ifndef DFTRACER_UTILS_RUNTIME_CORO_TASK_ABI_H
#define DFTRACER_UTILS_RUNTIME_CORO_TASK_ABI_H

#include <dftracer/utils/runtime/coro/abi.h>
#include <dftracer/utils/runtime/coro/task.h>

namespace dftracer::utils {

/// Take ownership of `task` and return it as an opaque dftu_task, freed when a
/// driver (dftu_task_run/when_all/when_any) runs it.
dftu_task* task_to_abi(coro::CoroTask<void>&& task);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_CORO_TASK_ABI_H
