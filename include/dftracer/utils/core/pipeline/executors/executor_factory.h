#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_FACTORY_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_FACTORY_H

#include <dftracer/utils/core/pipeline/executors/executor.h>
#include <dftracer/utils/core/pipeline/executors/executor_type.h>

#include <memory>

namespace dftracer::utils {

class ExecutorFactory {
   public:
    static std::shared_ptr<Executor> create_thread(std::size_t num_threads = 0);
    static std::shared_ptr<Executor> create_sequential();
    static std::shared_ptr<Executor> create(std::size_t num_threads = 0);

   private:
    static std::size_t get_default_thread_count();

   private:
    ExecutorFactory() = default;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_FACTORY_H
