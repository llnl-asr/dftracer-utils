// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/abi_error.h>
#include <dftracer/utils/coro/abi.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/task_abi.h>
#include <dftracer/utils/runtime/runtime.h>

using namespace dftracer::utils;

extern "C" {

dftu_runtime* dftu_default_runtime(void) {
    return reinterpret_cast<dftu_runtime*>(&default_runtime());
}

dftu_runtime* dftu_runtime_new(int32_t threads, int32_t io_threads) {
    RuntimeSettings settings;
    settings.threads = threads > 0 ? static_cast<std::size_t>(threads) : 0;
    settings.io_threads =
        io_threads > 0 ? static_cast<std::size_t>(io_threads) : 0;
    return reinterpret_cast<dftu_runtime*>(
        &configure_default_runtime(settings));
}

void dftu_runtime_free(dftu_runtime* rt) { (void)rt; }

dftu_status dftu_task_run(dftu_runtime* rt, dftu_task* t, dftu_error* err) {
    if (!t) {
        return common::fill_error(
            err, dstd::Error(dstd::Condition::InvalidArgument, "task is NULL"));
    }
    auto* runtime = rt ? reinterpret_cast<Runtime*>(rt) : &default_runtime();
    coro::AbiTask* task = coro::abi_task(t);
    const auto ran = runtime->block_on(
        coro::named("dftu_task_run", coro::run_abi_task(task)));
    dftu_status status = DFTU_OK;
    if (!ran) {
        status = common::fill_error(err, ran.error());
    } else if (task->status < 0) {
        status = common::fill_error(err, coro::abi_task_error(*task));
    } else {
        status = task->status;
    }
    coro::free_abi_task(t);
    return status;
}

}  // extern "C"
