// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_ERROR_H
#define DFTRACER_UTILS_COMMON_ERROR_H

#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdarg>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace dftracer::utils::common {

/// Coarse error category
enum class ErrorCode {
    Unknown,          ///< unclassified
    Internal,         ///< broken invariant / bug
    InvalidArgument,  ///< bad caller input
    NotFound,         ///< missing file / key / entity
    Io,               ///< filesystem / I/O failure
    Parse,            ///< parse / decode failure (JSON, format, ...)
    Compression,      ///< (de)compression failure / corrupt compressed data
    Duql,             ///< duql error
    Reader,           ///< reader subsystem
    Indexer,          ///< indexer subsystem
    Pipeline,         ///< pipeline / executor
    Aggregation,      ///< aggregation subsystem
    MemoryBudgetExceeded,  ///< a reservation does not fit the memory budget
};

inline constexpr dstd::ErrorCodeInfo CORE_ERROR_CODE_INFOS[] = {
    {&dstd::CORE_ERROR_DOMAIN, static_cast<std::int32_t>(ErrorCode::Unknown),
     dstd::Condition::Unknown, "UNKNOWN"},
    {&dstd::CORE_ERROR_DOMAIN, static_cast<std::int32_t>(ErrorCode::Internal),
     dstd::Condition::Internal, "INTERNAL"},
    {&dstd::CORE_ERROR_DOMAIN,
     static_cast<std::int32_t>(ErrorCode::InvalidArgument),
     dstd::Condition::InvalidArgument, "INVALID_ARGUMENT"},
    {&dstd::CORE_ERROR_DOMAIN, static_cast<std::int32_t>(ErrorCode::NotFound),
     dstd::Condition::NotFound, "NOT_FOUND"},
    {&dstd::CORE_ERROR_DOMAIN, static_cast<std::int32_t>(ErrorCode::Io),
     dstd::Condition::Io, "IO"},
    {&dstd::CORE_ERROR_DOMAIN, static_cast<std::int32_t>(ErrorCode::Parse),
     dstd::Condition::Parse, "PARSE"},
    {&dstd::CORE_ERROR_DOMAIN,
     static_cast<std::int32_t>(ErrorCode::Compression),
     dstd::Condition::Compression, "COMPRESSION"},
    {&dstd::CORE_ERROR_DOMAIN, static_cast<std::int32_t>(ErrorCode::Duql),
     dstd::Condition::InvalidArgument, "DUQL"},
    {&dstd::CORE_ERROR_DOMAIN, static_cast<std::int32_t>(ErrorCode::Reader),
     dstd::Condition::Internal, "READER"},
    {&dstd::CORE_ERROR_DOMAIN, static_cast<std::int32_t>(ErrorCode::Indexer),
     dstd::Condition::Internal, "INDEXER"},
    {&dstd::CORE_ERROR_DOMAIN, static_cast<std::int32_t>(ErrorCode::Pipeline),
     dstd::Condition::Internal, "PIPELINE"},
    {&dstd::CORE_ERROR_DOMAIN,
     static_cast<std::int32_t>(ErrorCode::Aggregation),
     dstd::Condition::Internal, "AGGREGATION"},
    {&dstd::CORE_ERROR_DOMAIN,
     static_cast<std::int32_t>(ErrorCode::MemoryBudgetExceeded),
     dstd::Condition::Internal, "MEMORY_BUDGET_EXCEEDED"},
};

constexpr const dstd::ErrorCodeInfo& error_info(ErrorCode code) noexcept {
    const auto at = static_cast<std::size_t>(code);
    return at < std::size(CORE_ERROR_CODE_INFOS) ? CORE_ERROR_CODE_INFOS[at]
                                                 : CORE_ERROR_CODE_INFOS[0];
}

inline const char* error_code_name(ErrorCode code) noexcept {
    return error_info(code).name.data();
}

constexpr dstd::Condition condition_of(ErrorCode code) noexcept {
    return error_info(code).condition;
}

/// The closest ErrorCode of a condition, or Unknown when none matches.
constexpr ErrorCode errorcode_of(dstd::Condition c) noexcept {
    switch (c) {
        case dstd::Condition::Unknown:
            return ErrorCode::Unknown;
        case dstd::Condition::Internal:
            return ErrorCode::Internal;
        case dstd::Condition::InvalidArgument:
            return ErrorCode::InvalidArgument;
        case dstd::Condition::NotFound:
            return ErrorCode::NotFound;
        case dstd::Condition::Io:
            return ErrorCode::Io;
        case dstd::Condition::Parse:
            return ErrorCode::Parse;
        case dstd::Condition::Compression:
            return ErrorCode::Compression;
        case dstd::Condition::Timeout:
        case dstd::Condition::Unsupported:
        case dstd::Condition::Cancelled:
            return ErrorCode::Unknown;
    }
    return ErrorCode::Unknown;
}

/// The ErrorCode of a core-domain error, else the equivalent of its
/// condition.
inline ErrorCode errorcode_of(const dstd::Error& error) noexcept {
    const auto code = static_cast<ErrorCode>(error.code());
    if (error.code() >= 0 &&
        static_cast<std::size_t>(error.code()) <
            std::size(CORE_ERROR_CODE_INFOS) &&
        error.is_code(code))
        return code;
    return errorcode_of(error.condition());
}

/// dstd::Unexpected converts to any dstd::Result<T>, so no type argument is
/// needed.
inline dstd::Unexpected<dstd::Error> make_error(ErrorCode code,
                                                dstd::StringView message) {
    return dstd::Unexpected<dstd::Error>(dstd::Error{code, message});
}

/// Build an error from a domain enum. The enum type selects its error_info via
/// ADL, so a code cannot be paired with the wrong domain. The result converts
/// to any dstd::Result<T>.
template <dstd::ErrorEnum E>
dstd::Unexpected<dstd::Error> make_error(E code, dstd::StringView message) {
    return dstd::Unexpected<dstd::Error>(dstd::Error{code, message});
}

/// Throwable carrying the (domain, condition) identity of a failure, so a
/// domain error throws without collapsing to a coarse code.
class DFTUtilsException : public std::exception {
   public:
    DFTUtilsException(ErrorCode code, dstd::StringView message)
        : error_(code, message) {}
    explicit DFTUtilsException(const dstd::Error& error) : error_(error) {}

    const char* what() const noexcept override { return error_.c_str(); }

    /// Concatenation factory (numbers via to_chars; no format string):
    ///   throw DFTUtilsException::cat(ErrorCode::IO,
    ///                                "Cannot open ", path, ": errno=", e);
    template <typename... Args>
    static DFTUtilsException cat(ErrorCode code, const Args&... args) {
        return DFTUtilsException(code, str_cat(args...));
    }

    /// printf-style factory:
    ///   throw DFTUtilsException::fmt(ErrorCode::IO,
    ///                                "Cannot open %s: errno=%d", path.c_str(),
    ///                                e);
    /// For a literal '%' in the message, escape as "%%" or use cat().
    DFTRACER_UTILS_PRINTF_FORMAT(2, 3)
    static DFTUtilsException fmt(ErrorCode code, const char* format, ...) {
        va_list ap;
        va_start(ap, format);
        dstd::String msg = vstring_format(format, ap);
        va_end(ap);
        return DFTUtilsException(code, std::move(msg));
    }

    std::uint64_t domain() const noexcept { return error_.domain().id; }
    dstd::StringView domain_name() const noexcept {
        return error_.domain().name;
    }
    std::int32_t code_int() const noexcept { return error_.code(); }
    dstd::Condition condition() const noexcept { return error_.condition(); }
    const dstd::Error& error() const noexcept { return error_; }

   private:
    dstd::Error error_;
};

/// The value of `r`, or throws @ref DFTUtilsException with its error.
template <typename T>
T unwrap(dstd::Result<T>&& r) {
    if (!r) throw DFTUtilsException(r.error());
    if constexpr (!std::is_void_v<T>) return std::move(*r);
}

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_ERROR_H
