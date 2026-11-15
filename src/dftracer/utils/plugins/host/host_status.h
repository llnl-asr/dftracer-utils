// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_HOST_STATUS_H
#define DFTRACER_UTILS_PLUGINS_HOST_HOST_STATUS_H

#include <dftracer/utils/common/c_api.h>
#include <dftracer/utils/common/c_api_error.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>

namespace dftracer::utils::plugins::host {

/// Reports @p message on the caller's @p ctx and returns @p status; a service
/// function returns this for every failure.
inline dftu_status host_fail(::dftu_ctx* ctx, dftu_status status,
                             dstd::StringView message) {
    if (ctx == nullptr) return status;
    return ctx->vt->set_error(ctx, status, message.data(),
                              static_cast<std::uint32_t>(message.size()));
}

inline dftu_status host_fail(::dftu_ctx* ctx, const dstd::Error& error) {
    return host_fail(ctx, common::to_status(error.condition()),
                     error.message());
}

/// The error for a plugin callback that returned the negative @p status;
/// @p message is what the plugin set on its ctx.
inline dstd::Error callback_error(dstd::StringView plugin,
                                  dstd::StringView callback, dftu_status status,
                                  dstd::StringView message) {
    if (message.empty()) message = "failed";
    return dstd::Error::from_format<"plugin '{}' {}: {}">(
        common::to_condition(status), plugin, callback, message);
}

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_HOST_STATUS_H
