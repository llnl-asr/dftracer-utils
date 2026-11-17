// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/c_api_error.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/scope.h>
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
        if (task->cancel_requested.load(std::memory_order_acquire)) {
            task->fail(DFTU_E_CANCELLED, "task cancelled");
            task->finished.store(true, std::memory_order_release);
            co_return task->status;
        }
        if (task->fast) {
            if (task->fast->now(task)) {
                task->finished.store(true, std::memory_order_release);
                co_return task->status;
            }
            task->body.emplace(task->fast->later(task));
        }
        coro::Executor* executor = co_await coro::this_coro::executor;
        task->executor.store(executor);
        task->body->handle().promise().cancel_link = &task->link;
        auto settled = co_await coro::settle(std::move(*task->body));
        if (settled.error) {
            task->fail(common::to_status(settled.error->condition()),
                       settled.error->message());
        } else if (settled.stopped && task->status >= 0) {
            task->stopped =
                !task->cancel_requested.load(std::memory_order_acquire);
            task->fail(DFTU_E_CANCELLED, "task cancelled");
        }
        task->finished.store(true, std::memory_order_release);
    }
    co_return task->status;
}

dstd::Error abi_task_error(const AbiTask& task) {
    const dstd::Condition condition = common::to_condition(task.status);
    if (task.message.empty()) return dstd::Error(condition);
    return dstd::Error(condition, task.message);
}

void cancel_abi_task(dftu_task* task) noexcept {
    AbiTask* t = abi_task(task);
    if (t->finished.load(std::memory_order_acquire)) return;
    t->cancel_requested.store(true);
    coro::Executor* executor = t->executor.load();
    if (executor != nullptr) executor->wake_cancelled();
}

void retain_abi_task(dftu_task* task) noexcept {
    abi_task(task)->refs.fetch_add(1, std::memory_order_relaxed);
}

bool abi_task_finished(const dftu_task* task) noexcept {
    return reinterpret_cast<const AbiTask*>(task)->finished.load(
        std::memory_order_acquire);
}

void free_abi_task(dftu_task* task) noexcept {
    AbiTask* t = abi_task(task);
    if (t->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        dstd::UniquePtr<AbiTask>(t).reset();
    }
}

}  // namespace dftracer::utils::plugins
