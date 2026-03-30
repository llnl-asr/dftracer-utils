#include <dftracer/utils/core/io/io_thread_pool.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/sqlite/async.h>

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

void sqlite_async_resume_on(void *executor, std::coroutine_handle<> h) {
    auto *exec = static_cast<Executor *>(executor);
    if (exec != nullptr) {
        exec->enqueue(h);
    } else {
        h.resume();
    }
}

void *get_current_executor_opaque() {
    return static_cast<void *>(Executor::current());
}

}  // namespace dftracer::utils::sqlite
