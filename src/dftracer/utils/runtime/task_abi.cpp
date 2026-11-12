// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/abi.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/task_abi.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/coro/when_any.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>

#include <exception>
#include <memory>
#include <utility>

using namespace dftracer::utils;

namespace dftracer::utils {

dftu_task* task_to_abi(coro::CoroTask<void>&& task) {
    return reinterpret_cast<dftu_task*>(
        dstd::make_unique<coro::CoroTask<void>>(std::move(task)).release());
}

dstd::UniquePtr<coro::CoroTask<void>> task_from_abi(dftu_task* task) noexcept {
    return dstd::UniquePtr<coro::CoroTask<void>>(
        reinterpret_cast<coro::CoroTask<void>*>(task));
}

namespace {

dstd::Vector<coro::CoroTask<void>> take(dftu_task* const* ts, std::uint32_t n) {
    dstd::Vector<coro::CoroTask<void>> out;
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        out.push_back(std::move(*task_from_abi(ts[i])));
    }
    return out;
}

// How an input ended. A task that threw is settled here, so no exception
// unwinds through a frame built without exceptions.
struct Outcome {
    bool stopped = false;
    std::exception_ptr exception;
};

// when_any cannot hold a void result, so race valued tasks and discard it.
coro::CoroTask<Outcome> as_outcome(coro::CoroTask<void> t) {
    auto settled = co_await coro::settle(std::move(t));
    co_return Outcome{settled.stopped, std::move(settled.exception)};
}

// A task the host owns ends the call with -1 when it stops or raises.
coro::CoroTask<void> await_one(coro::CoroTask<void>* task, int* rc) {
    auto settled = co_await coro::settle(std::move(*task));
    *rc = settled.stopped || settled.exception ? -1 : 0;
}

coro::CoroTask<void> await_all(dstd::Vector<coro::CoroTask<void>> inner) {
    dstd::Vector<coro::CoroTask<Outcome>> outcomes;
    outcomes.reserve(inner.size());
    for (auto& t : inner) outcomes.push_back(as_outcome(std::move(t)));
    auto done = co_await coro::when_all(std::move(outcomes));
    for (auto& outcome : done) {
        if (outcome.exception) {
            co_await coro::fail_with(std::move(outcome.exception));
        }
    }
    for (auto& outcome : done) {
        if (outcome.stopped) co_await coro::stop_here();
    }
}

coro::CoroTask<void> await_any(dstd::Vector<coro::CoroTask<void>> inner) {
    dstd::Vector<coro::CoroTask<Outcome>> outcomes;
    outcomes.reserve(inner.size());
    for (auto& t : inner) outcomes.push_back(as_outcome(std::move(t)));
    auto first = co_await coro::when_any(std::move(outcomes));
    if (first.result.exception) {
        co_await coro::fail_with(std::move(first.result.exception));
    }
    if (first.result.stopped) co_await coro::stop_here();
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
    int rc = -1;
    const auto ran = runtime->block_on(
        coro::named("dftu_task_run", [&]() -> coro::CoroTask<void> {
            return dftracer::utils::await_one(task, &rc);
        }()));
    if (!ran) rc = -1;
    dstd::UniquePtr<coro::CoroTask<void>>(task).reset();
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
