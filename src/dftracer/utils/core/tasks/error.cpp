#include <dftracer/utils/core/tasks/error.h>

namespace dftracer::utils {

std::string PipelineError::format_message(Type type,
                                          const std::string &message) {
    std::string prefix;
    switch (type) {
        case TypeMismatch:
            prefix = "[TYPE_MISMATCH]";
            break;
        case TypeMismatchError:
            prefix = "[TYPE_MISMATCH_ERROR]";
            break;
        case ValidationError:
            prefix = "[VALIDATION]";
            break;
        case ExecutionError:
            prefix = "[EXECUTION]";
            break;
        case InitializationError:
            prefix = "[INITIALIZATION]";
            break;
        case OutputConversionError:
            prefix = "[OUTPUT_CONVERSION]";
            break;
        case TimeoutError:
            prefix = "[TIMEOUT]";
            break;
        case Interrupted:
            prefix = "[INTERRUPTED]";
            break;
        case ExecutorUnresponsive:
            prefix = "[EXECUTOR_UNRESPONSIVE]";
            break;
        case UnknownError:
            prefix = "[UNKNOWN]";
            break;
    }
    return prefix + " " + message;
}

}  // namespace dftracer::utils
