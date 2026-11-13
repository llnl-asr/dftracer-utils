// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/c_api_error.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/plugins/task_abi.h>

#include <utility>

namespace dftracer::utils::plugins {

dftu_task* task_to_abi(coro::CoroTask<void>&& body) {
    auto task = dstd::make_unique<AbiTask>();
    task->body.emplace(std::move(body));
    return reinterpret_cast<dftu_task*>(task.release());
}

coro::CoroTask<dftu_status> run_abi_task(AbiTask* task) {
    if (!task->awaited) {
        task->awaited = true;
        auto settled = co_await coro::settle(std::move(*task->body));
        if (settled.error) {
            task->fail(common::to_status(settled.error->condition()),
                       settled.error->message());
        } else if (settled.stopped && task->status >= 0) {
            task->stopped = true;
            task->fail(DFTU_E_CANCELLED, "task cancelled");
        }
        task->finished = true;
    }
    co_return task->status;
}

dstd::Error abi_task_error(const AbiTask& task) {
    const dstd::Condition condition = common::to_condition(task.status);
    if (task.message.empty()) return dstd::Error(condition);
    return dstd::Error(condition, task.message);
}

void free_abi_task(dftu_task* task) noexcept {
    dstd::UniquePtr<AbiTask>(abi_task(task)).reset();
}

}  // namespace dftracer::utils::plugins
