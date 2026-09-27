#ifndef DFTRACER_UTILS_CORE_COMMON_ERROR_H
#define DFTRACER_UTILS_CORE_COMMON_ERROR_H

#include <dftracer/utils/core/common/expected.h>
#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/core/common/str_format.h>

#include <concepts>
#include <cstdarg>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace dftracer::utils {

/// Coarse error category
enum class ErrorCode {
    UNKNOWN,           ///< unclassified
    INTERNAL,          ///< broken invariant / bug
    INVALID_ARGUMENT,  ///< bad caller input
    NOT_FOUND,         ///< missing file / key / entity
    IO,                ///< filesystem / I/O failure
    PARSE,             ///< parse / decode failure (JSON, format, ...)
    COMPRESSION,       ///< (de)compression failure / corrupt compressed data
    DUQL,              ///< duql error
    READER,            ///< reader subsystem
    INDEXER,           ///< indexer subsystem
    PIPELINE,          ///< pipeline / executor
    AGGREGATION,       ///< aggregation subsystem
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
        case ErrorCode::COMPRESSION:
            return "COMPRESSION";
        case ErrorCode::DUQL:
            return "DUQL";
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

// Extensible error identity: (domain, code) is the precise identity, Condition
// the portable cross-domain match key. A library adds codes in its own headers
// (its own enum + a domain) without editing this file.

/// A subsystem's error domain: the FNV-1a `id` is the identity (matching, the C
/// ABI); `name` rides along because the hash cannot be reversed for printing.
struct ErrorDomain {
    std::uint64_t id = 0;
    std::string_view name;

    bool operator==(const ErrorDomain& o) const noexcept { return id == o.id; }
};

constexpr ErrorDomain make_error_domain(std::string_view name) noexcept {
    return ErrorDomain{hash::fnv1a_hash(name), name};
}

inline constexpr ErrorDomain CORE_DOMAIN = make_error_domain("dftracer.core");

/// Portable, cross-domain error category: callers match (and exhaustively
/// switch) on this without learning any domain's private codes. Closed on
/// purpose - keep it small.
enum class Condition : std::uint16_t {
    Unknown,
    Internal,
    InvalidArgument,
    NotFound,
    Io,
    Parse,
    Compression,
    Timeout,
    Unsupported,
    Cancelled,
};

constexpr const char* condition_name(Condition c) noexcept {
    switch (c) {
        case Condition::Unknown:
            return "UNKNOWN";
        case Condition::Internal:
            return "INTERNAL";
        case Condition::InvalidArgument:
            return "INVALID_ARGUMENT";
        case Condition::NotFound:
            return "NOT_FOUND";
        case Condition::Io:
            return "IO";
        case Condition::Parse:
            return "PARSE";
        case Condition::Compression:
            return "COMPRESSION";
        case Condition::Timeout:
            return "TIMEOUT";
        case Condition::Unsupported:
            return "UNSUPPORTED";
        case Condition::Cancelled:
            return "CANCELLED";
    }
    return "UNKNOWN";
}

/// Legacy ErrorCode -> portable condition (subsystem codes fold to Internal).
constexpr Condition condition_of(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::UNKNOWN:
            return Condition::Unknown;
        case ErrorCode::INTERNAL:
            return Condition::Internal;
        case ErrorCode::INVALID_ARGUMENT:
            return Condition::InvalidArgument;
        case ErrorCode::NOT_FOUND:
            return Condition::NotFound;
        case ErrorCode::IO:
            return Condition::Io;
        case ErrorCode::PARSE:
            return Condition::Parse;
        case ErrorCode::COMPRESSION:
            return Condition::Compression;
        case ErrorCode::DUQL:
            return Condition::InvalidArgument;
        case ErrorCode::READER:
        case ErrorCode::INDEXER:
        case ErrorCode::PIPELINE:
        case ErrorCode::AGGREGATION:
            return Condition::Internal;
    }
    return Condition::Unknown;
}

/// Condition -> best-effort legacy ErrorCode, for the back-compat code()
/// accessor on a domain error; conditions with no equivalent fold to UNKNOWN.
constexpr ErrorCode errorcode_of(Condition c) noexcept {
    switch (c) {
        case Condition::Unknown:
            return ErrorCode::UNKNOWN;
        case Condition::Internal:
            return ErrorCode::INTERNAL;
        case Condition::InvalidArgument:
            return ErrorCode::INVALID_ARGUMENT;
        case Condition::NotFound:
            return ErrorCode::NOT_FOUND;
        case Condition::Io:
            return ErrorCode::IO;
        case Condition::Parse:
            return ErrorCode::PARSE;
        case Condition::Compression:
            return ErrorCode::COMPRESSION;
        case Condition::Timeout:
        case Condition::Unsupported:
        case Condition::Cancelled:
            return ErrorCode::UNKNOWN;
    }
    return ErrorCode::UNKNOWN;
}

/// Error as a value (travels inside Result<T>); see DFTUtilsException to throw.
struct DFTUtilsError {
    ErrorCode code = ErrorCode::UNKNOWN;
    std::string message;

    DFTUtilsError() = default;
    DFTUtilsError(ErrorCode c, std::string msg)
        : code(c), message(std::move(msg)) {}

    /// "<CODE>: <message>"
    std::string format() const {
        std::string out = error_code_name(code);
        out += ": ";
        out += message;
        return out;
    }
};

/// Recoverable-failure channel; reserve exceptions for the unrecoverable.
template <typename T>
using Result = expected<T, DFTUtilsError>;

/// unexpected converts to any Result<T>, so no type argument is needed.
inline unexpected<DFTUtilsError> make_error(ErrorCode code,
                                            std::string message) {
    return unexpected<DFTUtilsError>(DFTUtilsError{code, std::move(message)});
}

/// Extensible error value: (domain, code) identity, `condition` the portable
/// match key, `message` the only allocation. Built with make_error(E); travels
/// inside ErrorOr<T> and needs no exception machinery.
struct Error {
    std::uint64_t domain = CORE_DOMAIN.id;
    std::string_view domain_name = CORE_DOMAIN.name;
    std::int32_t code = static_cast<std::int32_t>(Condition::Unknown);
    Condition condition = Condition::Unknown;
    std::string message;

    bool is(Condition c) const noexcept { return condition == c; }
    bool in_domain(const ErrorDomain& d) const noexcept {
        return domain == d.id;
    }

    /// "<domain>#<code> <CONDITION>: <message>"
    std::string format() const {
        std::string out(domain_name);
        out += '#';
        out += std::to_string(code);
        out += ' ';
        out += condition_name(condition);
        out += ": ";
        out += message;
        return out;
    }
};

/// A per-domain enum opts in by defining, in its own namespace (found by ADL),
/// error_domain(E) -> ErrorDomain and error_condition(E) -> Condition.
template <class E>
concept ErrorEnum = std::is_enum_v<E> && requires(E e) {
    { error_domain(e) } -> std::convertible_to<ErrorDomain>;
    { error_condition(e) } -> std::convertible_to<Condition>;
};

/// Build an Error from a domain enum; domain and condition are deduced from the
/// enum type, so a code cannot be paired with the wrong domain and a typo does
/// not compile. Wrap in unexpected(...) to return from an ErrorOr<T>.
template <ErrorEnum E>
Error make_error(E code, std::string message) {
    const ErrorDomain d = error_domain(code);
    return Error{d.id, d.name, static_cast<std::int32_t>(code),
                 static_cast<Condition>(error_condition(code)),
                 std::move(message)};
}

/// Core-domain convenience: a bare Condition is a core error (code == the
/// condition), for a generic failure needing no domain-specific code.
inline Error make_error(Condition c, std::string message) {
    return Error{CORE_DOMAIN.id, CORE_DOMAIN.name, static_cast<std::int32_t>(c),
                 c, std::move(message)};
}

/// Exception-free result channel carrying the extensible Error.
template <typename T>
using ErrorOr = expected<T, Error>;

/// Throwable carrying an ErrorCode (so any catch site, notably the Python
/// boundary, can inspect code()) plus the extensible (domain, condition)
/// identity, so a domain error throws without collapsing to a legacy code.
class DFTUtilsException : public std::runtime_error {
   public:
    DFTUtilsException(ErrorCode code, const std::string& message)
        : std::runtime_error(message),
          code_(code),
          domain_(CORE_DOMAIN.id),
          domain_name_(CORE_DOMAIN.name),
          code_int_(static_cast<std::int32_t>(code)),
          condition_(condition_of(code)) {}
    explicit DFTUtilsException(const DFTUtilsError& err)
        : DFTUtilsException(err.code, err.message) {}
    explicit DFTUtilsException(const Error& err)
        : std::runtime_error(err.message),
          code_(errorcode_of(err.condition)),
          domain_(err.domain),
          domain_name_(err.domain_name),
          code_int_(err.code),
          condition_(err.condition) {}

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
    __attribute__((__format__(__printf__, 2, 3))) static DFTUtilsException fmt(
        ErrorCode code, const char* format, ...) {
        va_list ap;
        va_start(ap, format);
        std::string msg = vstring_format(format, ap);
        va_end(ap);
        return DFTUtilsException(code, std::move(msg));
    }

    /// Legacy coarse code (core-domain proxy for a domain error).
    ErrorCode code() const noexcept { return code_; }
    std::uint64_t domain() const noexcept { return domain_; }
    std::string_view domain_name() const noexcept { return domain_name_; }
    std::int32_t code_int() const noexcept { return code_int_; }
    Condition condition() const noexcept { return condition_; }
    Error error() const {
        return Error{domain_, domain_name_, code_int_, condition_, what()};
    }

   private:
    ErrorCode code_;
    std::uint64_t domain_;
    std::string_view domain_name_;
    std::int32_t code_int_;
    Condition condition_;
};

}  // namespace dftracer::utils

/// DFTU_TRY: error-propagation helper for coroutines returning Result<...>.
/// Evaluates a Result-returning `expr`; on failure it co_returns the error,
/// otherwise it binds the moved-out value to `decl`. The temporary is named
/// with a line-derived suffix so multiple uses in one scope do not collide.
/// Usage: DFTU_TRY(auto value, co_await something_returning_result());
#define DFTU_TRY_CONCAT_(a, b) a##b
#define DFTU_TRY_CONCAT(a, b) DFTU_TRY_CONCAT_(a, b)
#define DFTU_TRY(decl, expr)                                          \
    auto DFTU_TRY_CONCAT(dftu_try_, __LINE__) = (expr);               \
    if (!DFTU_TRY_CONCAT(dftu_try_, __LINE__))                        \
        co_return ::dftracer::utils::unexpected(                      \
            std::move(DFTU_TRY_CONCAT(dftu_try_, __LINE__)).error()); \
    decl = *std::move(DFTU_TRY_CONCAT(dftu_try_, __LINE__))

#endif  // DFTRACER_UTILS_CORE_COMMON_ERROR_H
