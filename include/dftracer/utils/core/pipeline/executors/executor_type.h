#ifndef DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_TYPE_H
#define DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_TYPE_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace dftracer::utils {

enum class ExecutorType : std::uint8_t { SEQUENTIAL, THREAD };

std::string executor_type_to_string(ExecutorType type);
ExecutorType string_to_executor_type(const std::string &str);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_EXECUTORS_EXECUTOR_TYPE_H
