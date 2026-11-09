// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_PLUGIN_TASK_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_PLUGIN_TASK_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/task_abi.h>
#include <dftracer/utils/dstd/result.h>

namespace dftracer::utils::dataframe {

/// Awaits the task a plugin returned and frees it. A plugin may throw inside
/// its task, and an exception that unwinds through a frame built without
/// exceptions aborts, so the task is awaited through @ref coro::settle: a throw
/// becomes an error result and a stop ends the caller as stopped.
inline coro::CoroTask<dstd::Result<void>> await_plugin_task(::dftu_task* t) {
    auto task = task_from_abi(t);
    auto settled = co_await coro::settle_in_place(*task);
    if (settled.stopped) co_await coro::stop_here();
    if (settled.exception)
        co_return common::error_of_exception(settled.exception);
    co_return dstd::Result<void>{};
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_PLUGIN_TASK_H
