// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_PLUGIN_ERROR_H
#define DFTRACER_UTILS_DATAFRAME_PLUGIN_ERROR_H

// The one bridge between a @ref dstd::Error and the status the plugin C ABI
// carries. A UDF, a UDAF and the plugin SDK all fail the same way, so they all
// build their errors here and translate them here: a plugin author names a
// @ref dstd::Condition and never a DFTU_E_* constant.

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

/// An error of `condition`, with `message` when it is not empty. The error a
/// plugin callback hands back; the host reports its condition and message.
inline dstd::Error make_error(dstd::Condition condition,
                              dstd::StringView message = {}) noexcept {
    return dstd::Error(condition, message);
}

/// An error of @ref dstd::Condition "Condition::InvalidArgument".
inline dstd::Error invalid_argument(dstd::StringView message = {}) noexcept {
    return dstd::Error(dstd::Condition::InvalidArgument, message);
}

/// An error of @ref dstd::Condition "Condition::Io".
inline dstd::Error io_error(dstd::StringView message = {}) noexcept {
    return dstd::Error(dstd::Condition::Io, message);
}

/// An error of @ref dstd::Condition "Condition::Unsupported".
inline dstd::Error unsupported(dstd::StringView message = {}) noexcept {
    return dstd::Error(dstd::Condition::Unsupported, message);
}

/// An error of @ref dstd::Condition "Condition::Internal".
inline dstd::Error internal(dstd::StringView message = {}) noexcept {
    return dstd::Error(dstd::Condition::Internal, message);
}

/// An error of @ref dstd::Condition "Condition::NotFound".
inline dstd::Error not_found(dstd::StringView message = {}) noexcept {
    return dstd::Error(dstd::Condition::NotFound, message);
}

/// An error of @ref dstd::Condition "Condition::Parse".
inline dstd::Error parse_error(dstd::StringView message = {}) noexcept {
    return dstd::Error(dstd::Condition::Parse, message);
}

/// An error of @ref dstd::Condition "Condition::Cancelled".
inline dstd::Error cancelled(dstd::StringView message = {}) noexcept {
    return dstd::Error(dstd::Condition::Cancelled, message);
}

/// An error of @ref dstd::Condition "Condition::OutOfMemory".
inline dstd::Error out_of_memory(dstd::StringView message = {}) noexcept {
    return dstd::Error(dstd::Condition::OutOfMemory, message);
}

/// The error of a host call that returned `status`, which numbers the
/// conditions the way dftu_status does. A status that is not an error, or one
/// past the last condition, gives @ref dstd::Condition "Condition::Unknown".
inline dstd::Error error_of(dftu_status status,
                            dstd::StringView message = {}) noexcept {
    const std::int32_t code = -status - 1;
    const auto condition =
        status < 0 &&
                code <= static_cast<std::int32_t>(dstd::Condition::OutOfMemory)
            ? static_cast<dstd::Condition>(code)
            : dstd::Condition::Unknown;
    return dstd::Error(condition, message);
}

/// The status a callback returns for `error`.
///
/// A host error carries no message, because the host already holds it on the
/// call's dftu_ctx; @ref report then returns the status as it is.
inline dftu_status status_of(const dstd::Error& error) noexcept {
    return -static_cast<dftu_status>(error.condition()) - 1;
}

/// The status a callback returns for `error`, with its message set on `ctx`.
/// A null `ctx` takes only the status, and an error with no message leaves the
/// one the host already has.
inline dftu_status report(dftu_ctx* ctx, const dstd::Error& error) noexcept {
    const dftu_status status = status_of(error);
    return error.message().empty() || ctx == nullptr
               ? status
               : dftu_fail(ctx, status, error.c_str());
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_PLUGIN_ERROR_H
