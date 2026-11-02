// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/abi.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/task_abi.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/coro/when_any.h>
#include <dftracer/utils/runtime/runtime.h>

#include <memory>
#include <utility>
#include <vector>

using namespace dftracer::utils;

namespace dftracer::utils {

dftu_task* task_to_abi(coro::CoroTask<void>&& task) {
    return reinterpret_cast<dftu_task*>(
        new coro::CoroTask<void>(std::move(task)));
}

namespace {

std::vector<coro::CoroTask<void>> take(dftu_task* const* ts, std::uint32_t n) {
    std::vector<coro::CoroTask<void>> out;
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        auto* p = reinterpret_cast<coro::CoroTask<void>*>(ts[i]);
        out.push_back(std::move(*p));
        delete p;
    }
    return out;
}

// when_any cannot hold a void result, so race valued tasks and discard it.
coro::CoroTask<int> as_valued(coro::CoroTask<void> t) {
    co_await std::move(t);
    co_return 0;
}

coro::CoroTask<void> await_one(coro::CoroTask<void>* task) {
    co_await std::move(*task);
}

coro::CoroTask<void> await_all(std::vector<coro::CoroTask<void>> inner) {
    co_await coro::when_all(std::move(inner));
}

coro::CoroTask<void> await_any(std::vector<coro::CoroTask<void>> inner) {
    std::vector<coro::CoroTask<int>> valued;
    valued.reserve(inner.size());
    for (auto& t : inner) valued.push_back(as_valued(std::move(t)));
    co_await coro::when_any(std::move(valued));
}

}  // namespace

}  // namespace dftracer::utils

using dftracer::utils::configure_default_runtime;
using dftracer::utils::default_runtime;
using dftracer::utils::Runtime;
using dftracer::utils::RuntimeSettings;
using dftracer::utils::task_to_abi;

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

int dftu_task_run(dftu_runtime* rt, dftu_task* t) {
    if (!t) return -1;
    auto* runtime = rt ? reinterpret_cast<Runtime*>(rt) : &default_runtime();
    auto* task = reinterpret_cast<coro::CoroTask<void>*>(t);
    int rc = 0;
    try {
        runtime->block_on(
            coro::named("dftu_task_run", [&]() -> coro::CoroTask<void> {
                return dftracer::utils::await_one(task);
            }()));
    } catch (...) {
        rc = -1;
    }
    delete task;
    return rc;
}

dftu_task* dftu_task_when_all(dftu_task* const* ts, uint32_t n) {
    if (n == 0 || !ts) return nullptr;
    return task_to_abi(
        dftracer::utils::await_all(dftracer::utils::take(ts, n)));
}

dftu_task* dftu_task_when_any(dftu_task* const* ts, uint32_t n) {
    if (n == 0 || !ts) return nullptr;
    return task_to_abi(
        dftracer::utils::await_any(dftracer::utils::take(ts, n)));
}

}  // extern "C"
