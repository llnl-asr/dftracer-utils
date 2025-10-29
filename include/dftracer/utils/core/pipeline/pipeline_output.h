#ifndef DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_OUTPUT_H
#define DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_OUTPUT_H

#include <dftracer/utils/core/common/typedefs.h>
#include <dftracer/utils/core/pipeline/error.h>

#include <any>
#include <string>
#include <unordered_map>

namespace dftracer::utils {

struct PipelineOutput : public std::unordered_map<TaskIndex, std::any> {
    operator std::any() const {
        if (size() == 1) return begin()->second;
        throw PipelineError(PipelineError::OUTPUT_CONVERSION_ERROR,
                            "Cannot convert multi-output pipeline (" +
                                std::to_string(size()) +
                                " outputs) to single std::any");
    }

    std::any get() const {
        if (size() == 1) return begin()->second;
        throw PipelineError(
            PipelineError::OUTPUT_CONVERSION_ERROR,
            "Cannot get single value from multi-output pipeline (" +
                std::to_string(size()) + " outputs)");
    }

    template <typename T>
    T get() const {
        return std::any_cast<T>(get());
    }

    std::any get(TaskIndex id) const {
        auto it = find(id);
        if (it == end()) {
            throw PipelineError(
                PipelineError::OUTPUT_CONVERSION_ERROR,
                "No output found for task ID " + std::to_string(id));
        }
        return it->second;
    }

    template <typename T>
    T get(TaskIndex id) const {
        auto it = get(id);
        try {
            return std::any_cast<T>(it);
        } catch (const std::bad_any_cast& e) {
            throw PipelineError(PipelineError::OUTPUT_CONVERSION_ERROR,
                                "Failed to cast output of task ID " +
                                    std::to_string(id) + ": " + e.what());
        }
    }

    std::any first() const {
        if (empty())
            throw PipelineError(PipelineError::OUTPUT_CONVERSION_ERROR,
                                "No pipeline outputs available");
        return begin()->second;
    }

    template <typename T>
    T first() const {
        return std::any_cast<T>(first());
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_PIPELINE_OUTPUT_H
