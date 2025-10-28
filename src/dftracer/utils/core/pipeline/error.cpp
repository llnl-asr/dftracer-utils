#include <dftracer/utils/core/pipeline/error.h>

namespace dftracer::utils {

std::string PipelineError::format_message(Type type,
                                          const std::string &message) {
    std::string prefix;
    switch (type) {
        case TYPE_MISMATCH:
            prefix = "[TYPE_MISMATCH]";
            break;
        case TYPE_MISMATCH_ERROR:
            prefix = "[TYPE_MISMATCH_ERROR]";
            break;
        case VALIDATION_ERROR:
            prefix = "[VALIDATION]";
            break;
        case EXECUTION_ERROR:
            prefix = "[EXECUTION]";
            break;
        case INITIALIZATION_ERROR:
            prefix = "[INITIALIZATION]";
            break;
        case OUTPUT_CONVERSION_ERROR:
            prefix = "[OUTPUT_CONVERSION]";
            break;
        case TIMEOUT_ERROR:
            prefix = "[TIMEOUT]";
            break;
        case INTERRUPTED:
            prefix = "[INTERRUPTED]";
            break;
        case EXECUTOR_UNRESPONSIVE:
            prefix = "[EXECUTOR_UNRESPONSIVE]";
            break;
        case UNKNOWN_ERROR:
            prefix = "[UNKNOWN]";
            break;
    }
    return prefix + " " + message;
}

}  // namespace dftracer::utils
