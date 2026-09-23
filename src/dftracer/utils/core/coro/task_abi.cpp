#include <dftracer/utils/core/coro/abi.h>
#include <dftracer/utils/core/coro/task_abi.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/coro/when_any.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>

#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

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

using dftracer::utils::clear_default_runtime_if;
using dftracer::utils::CoroScope;
using dftracer::utils::default_runtime;
using dftracer::utils::ExecutorConfig;
using dftracer::utils::Runtime;
using dftracer::utils::set_default_runtime;
using dftracer::utils::task_to_abi;
using dftracer::utils::try_install_default_runtime;
using dftracer::utils::coro::CoroTask;

namespace {
std::size_t normalize_thread_count(std::int32_t v) {
    return v > 0 ? static_cast<std::size_t>(v) : 0;
}

// The strong reference the C ABI itself holds for each dftu_runtime_new. The
// runtime is shared: default_runtime_shared() hands copies out, and the Python
// Runtime object stores one that outlives the call. So free() drops THIS
// reference rather than deleting, and the object lives until the last holder
// releases it. A non-owning shared_ptr here would let dftu_runtime_free leave
// every other holder pointing at freed memory while their refcount still said
// the object was alive.
std::mutex& abi_runtime_mutex() {
    static std::mutex m;
    return m;
}
std::map<Runtime*, std::shared_ptr<Runtime>>& abi_runtimes() {
    static std::map<Runtime*, std::shared_ptr<Runtime>> m;
    return m;
}

}  // namespace

extern "C" {

dftu_runtime* dftu_default_runtime(void) {
    return reinterpret_cast<dftu_runtime*>(&default_runtime());
}

dftu_runtime* dftu_runtime_new(int32_t threads, int32_t io_threads) {
    ExecutorConfig cfg;
    cfg.num_threads = normalize_thread_count(threads);
    cfg.io_pool_size = normalize_thread_count(io_threads);
    auto rt = std::make_shared<Runtime>(cfg);
    Runtime* raw = rt.get();
    {
        std::lock_guard<std::mutex> lock(abi_runtime_mutex());
        abi_runtimes().emplace(raw, rt);
    }
    try_install_default_runtime(rt);
    return reinterpret_cast<dftu_runtime*>(raw);
}

void dftu_runtime_free(dftu_runtime* rt) {
    if (!rt) return;
    auto* runtime = reinterpret_cast<Runtime*>(rt);
    clear_default_runtime_if(runtime);
    std::shared_ptr<Runtime> dropped;  // released outside the lock
    {
        std::lock_guard<std::mutex> lock(abi_runtime_mutex());
        auto it = abi_runtimes().find(runtime);
        if (it != abi_runtimes().end()) {
            dropped = std::move(it->second);
            abi_runtimes().erase(it);
        }
    }
}

void dftu_set_default_runtime(dftu_runtime* rt) {
    if (!rt) {
        set_default_runtime(nullptr);
        return;
    }
    auto* runtime = reinterpret_cast<Runtime*>(rt);
    set_default_runtime(std::shared_ptr<Runtime>(runtime, [](Runtime*) {}));
}

int dftu_task_run(dftu_runtime* rt, dftu_task* t) {
    if (!t) return -1;
    auto* runtime = rt ? reinterpret_cast<Runtime*>(rt) : &default_runtime();
    auto* task = reinterpret_cast<CoroTask<void>*>(t);
    int rc = 0;
    try {
        runtime->run_blocking("dftu_task_run",
                              [&](CoroScope&) -> CoroTask<void> {
                                  return dftracer::utils::await_one(task);
                              });
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
