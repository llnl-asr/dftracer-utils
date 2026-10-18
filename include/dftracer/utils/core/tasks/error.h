#ifndef DFTRACER_UTILS_CORE_TASKS_ERROR_H
#define DFTRACER_UTILS_CORE_TASKS_ERROR_H

#include <dftracer/utils/common/error.h>

#include <stdexcept>
#include <string>

namespace dftracer::utils {

class PipelineError : public DFTUtilsException {
   public:
    enum Type {
        TypeMismatch,
        TypeMismatchError,
        ValidationError,
        ExecutionError,
        InitializationError,
        OutputConversionError,
        TimeoutError,          // Pipeline or task timeout
        Interrupted,           // Graceful shutdown requested
        ExecutorUnresponsive,  // Executor hung/crashed
        UnknownError,
    };

    PipelineError(Type type, const std::string &message)
        : DFTUtilsException(ErrorCode::Pipeline, format_message(type, message)),
          type_(type) {}

    inline Type get_type() const { return type_; }

   private:
    Type type_;

    static std::string format_message(Type type, const std::string &message);
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_TASKS_ERROR_H
