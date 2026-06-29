#ifndef DFTRACER_UTILS_CORE_COMMON_ERROR_H
#define DFTRACER_UTILS_CORE_COMMON_ERROR_H

#include <dftracer/utils/core/common/expected.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace dftracer::utils {

// Coarse error category
enum class ErrorCode {
    UNKNOWN,           // unclassified
    INTERNAL,          // broken invariant / bug
    INVALID_ARGUMENT,  // bad caller input
    NOT_FOUND,         // missing file / key / entity
    IO,                // filesystem / I/O failure
    PARSE,             // parse / decode failure (JSON, format, ...)
    QUERY,             // query DSL error
    READER,            // reader subsystem
    INDEXER,           // indexer subsystem
    PIPELINE,          // pipeline / executor
    AGGREGATION,       // aggregation subsystem
};

inline const char* error_code_name(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::UNKNOWN:
            return "UNKNOWN";
        case ErrorCode::INTERNAL:
            return "INTERNAL";
        case ErrorCode::INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case ErrorCode::NOT_FOUND:
            return "NOT_FOUND";
        case ErrorCode::IO:
            return "IO";
        case ErrorCode::PARSE:
            return "PARSE";
        case ErrorCode::QUERY:
            return "QUERY";
        case ErrorCode::READER:
            return "READER";
        case ErrorCode::INDEXER:
            return "INDEXER";
        case ErrorCode::PIPELINE:
            return "PIPELINE";
        case ErrorCode::AGGREGATION:
            return "AGGREGATION";
    }
    return "UNKNOWN";
}

// Error as a value (travels inside Result<T>); see DFTUtilsException to throw.
struct DFTUtilsError {
    ErrorCode code = ErrorCode::UNKNOWN;
    std::string message;

    DFTUtilsError() = default;
    DFTUtilsError(ErrorCode c, std::string msg)
        : code(c), message(std::move(msg)) {}

    // "<CODE>: <message>"
    std::string format() const {
        std::string out = error_code_name(code);
        out += ": ";
        out += message;
        return out;
    }
};

// Recoverable-failure channel; reserve exceptions for the unrecoverable.
template <typename T>
using Result = expected<T, DFTUtilsError>;

// unexpected converts to any Result<T>, so no type argument is needed.
inline unexpected<DFTUtilsError> make_error(ErrorCode code,
                                            std::string message) {
    return unexpected<DFTUtilsError>(DFTUtilsError{code, std::move(message)});
}

// Throwable carrying an ErrorCode; base of the domain exception classes so any
// catch site (notably the Python boundary) can inspect code().
class DFTUtilsException : public std::runtime_error {
   public:
    DFTUtilsException(ErrorCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    explicit DFTUtilsException(const DFTUtilsError& err)
        : std::runtime_error(err.message), code_(err.code) {}

    ErrorCode code() const noexcept { return code_; }

   private:
    ErrorCode code_;
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_ERROR_H
