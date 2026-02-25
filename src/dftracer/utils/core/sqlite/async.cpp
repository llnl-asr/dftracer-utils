#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/sqlite/async.h>

#include "../io/io_thread_pool.h"

namespace dftracer::utils::sqlite {

io::IoThreadPool *get_sqlite_pool() {
    auto *exec = Executor::current();
    if (exec == nullptr) {
        return nullptr;
    }
    return exec->sqlite_pool();
}

void sqlite_async_submit(io::IoThreadPool *pool, std::function<void()> fn) {
    pool->submit(std::move(fn));
}

void sqlite_async_resume(std::coroutine_handle<> h) {
    auto *exec = Executor::current();
    if (exec != nullptr) {
        exec->enqueue(h);
    } else {
        h.resume();
    }
}

}  // namespace dftracer::utils::sqlite
