#ifndef DFTRACER_UTILS_CORE_PIPELINE_ERROR_H
#define DFTRACER_UTILS_CORE_PIPELINE_ERROR_H

#include <dftracer/utils/core/common/error.h>

#include <stdexcept>
#include <string>

namespace dftracer::utils {

class PipelineError : public DFTUtilsException {
   public:
    enum Type {
        TYPE_MISMATCH,
        TYPE_MISMATCH_ERROR,
        VALIDATION_ERROR,
        EXECUTION_ERROR,
        INITIALIZATION_ERROR,
        OUTPUT_CONVERSION_ERROR,
        TIMEOUT_ERROR,          // Pipeline or task timeout
        INTERRUPTED,            // Graceful shutdown requested
        EXECUTOR_UNRESPONSIVE,  // Executor hung/crashed
        UNKNOWN_ERROR,
    };

    PipelineError(Type type, const std::string &message)
        : DFTUtilsException(ErrorCode::PIPELINE, format_message(type, message)),
          type_(type) {}

    inline Type get_type() const { return type_; }

   private:
    Type type_;

    static std::string format_message(Type type, const std::string &message);
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_PIPELINE_ERROR_H
