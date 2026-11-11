// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_ERROR_H
#define DFTRACER_UTILS_COMMON_ERROR_H

#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>
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
        case dstd::Condition::ForkedHandle:
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

/// An error whose message is the format `S` filled with `a...`, built with
/// one allocation and no temporary string:
///   return common::make_error<"open {}: {}">(ErrorCode::Io, path, reason);
/// The format is checked at compile time (see @ref dstd::fmt). A helper that
/// takes a message writes itself once as
///   template <dstd::FixedString S, dstd::FmtArg... A>
///   auto fail(const A&... a) { return common::make_error<S>(code, a...); }
/// and, for the `fail("..."_f, x)` call form,
///   template <dstd::FixedString S, dstd::FmtArg... A>
///   auto fail(dstd::Format<S> f, const A&... a) { return
///   common::make_error(code, f, a...); }
/// A generic lambda cannot take `<"...">`, so it takes the format as a value:
///   auto fail = [&](auto f, const auto&... a) { return
///   common::make_error(code, f, a...); };
template <dstd::FixedString S, dstd::ErrorEnum E, dstd::FmtArg... A>
dstd::Unexpected<dstd::Error> make_error(E code, const A&... a) {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error::from_format<S>(code, a...));
}

/// @ref make_error with the format as a value: `make_error(code, "x {}"_f, v)`.
template <dstd::ErrorEnum E, dstd::FixedString S, dstd::FmtArg... A>
dstd::Unexpected<dstd::Error> make_error(E code, dstd::Format<S> f,
                                         const A&... a) {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error::from_format(code, f, a...));
}

/// The error an exception stands for: the what() of a std::exception as
/// dstd::Condition::Unknown, and "unknown error" for anything else. Defined in
/// a source built with exceptions, so code that cannot catch can still read an
/// exception.
dstd::Error error_of_exception(const std::exception_ptr& error) noexcept;

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_ERROR_H
