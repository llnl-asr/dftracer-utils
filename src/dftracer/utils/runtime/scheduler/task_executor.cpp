#include <dftracer/utils/runtime/scheduler/task_executor.h>
#include <dftracer/utils/runtime/scheduler/thread_pool_executor.h>

#include <memory>

namespace dftracer::utils {

static thread_local void* tls_current_worker_context = nullptr;

void* get_current_worker_context() { return tls_current_worker_context; }

void set_current_worker_context(void* context) {
    tls_current_worker_context = context;
}

std::unique_ptr<TaskExecutor> make_task_executor(const ExecutorConfig& config) {
    return std::make_unique<ThreadPoolExecutor>(config);
}

}  // namespace dftracer::utils
