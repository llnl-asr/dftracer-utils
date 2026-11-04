// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/runtime/scheduler/task_executor.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>

#include <memory>

namespace dftracer::utils {

static thread_local void* t_current_worker_context = nullptr;

void* get_current_worker_context() { return t_current_worker_context; }

void set_current_worker_context(void* context) {
    t_current_worker_context = context;
}

void TaskExecutor::set_current_backend(TaskExecutor* executor) noexcept {
    const bool has_backend = executor != nullptr && executor->has_io_backend();
    io::set_current_backend(executor,
                            has_backend ? &executor->io_backend() : nullptr);
}

void mark_coro_failed(TaskExecutor& executor, common::TaskIndex id,
                      std::exception_ptr error) noexcept {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        executor.mark_coro_failed(id, e.what());
    } catch (...) {
        executor.mark_coro_failed(id, "Unknown exception");
    }
}

dstd::StringView task_state_name(TaskState state) noexcept {
    switch (state) {
        case TaskState::Queued:
            return "queued";
        case TaskState::Running:
            return "running";
        case TaskState::Waiting:
            return "waiting";
        case TaskState::Completed:
            return "completed";
        case TaskState::Failed:
            return "failed";
    }
    return {};
}

dstd::UniquePtr<TaskExecutor> make_task_executor(const ExecutorConfig& config) {
    return dstd::make_unique<ThreadPoolExecutor>(config);
}

}  // namespace dftracer::utils
