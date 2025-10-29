#include <dftracer/utils/core/pipeline/executors/executor.h>
#include <dftracer/utils/core/pipeline/executors/executor_factory.h>
#include <dftracer/utils/core/pipeline/executors/sequential_executor.h>
#include <dftracer/utils/core/pipeline/executors/thread_executor.h>

#include <stdexcept>
#include <thread>

namespace dftracer::utils {

std::shared_ptr<Executor> ExecutorFactory::create_thread(
    std::size_t num_threads) {
    if (num_threads == 0) {
        num_threads = get_default_thread_count();
    }

    return std::make_shared<ThreadExecutor>(num_threads);
}

std::shared_ptr<Executor> ExecutorFactory::create_sequential() {
    return std::make_shared<SequentialExecutor>();
}

std::shared_ptr<Executor> ExecutorFactory::create(std::size_t num_threads) {
    if (num_threads == 0 || num_threads == 1) {
        return create_sequential();
    } else {
        return create_thread(num_threads);
    }
}

std::size_t ExecutorFactory::get_default_thread_count() {
    std::size_t hardware_threads = std::thread::hardware_concurrency();

    // Fall back to 4 threads if hardware_concurrency() returns 0
    // (which can happen on some systems where this information is not
    // available)
    if (hardware_threads == 0) {
        return 4;
    }

    return hardware_threads;
}

}  // namespace dftracer::utils
