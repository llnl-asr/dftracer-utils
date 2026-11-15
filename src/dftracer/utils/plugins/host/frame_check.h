// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_FRAME_CHECK_H
#define DFTRACER_UTILS_PLUGINS_HOST_FRAME_CHECK_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/validate.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::plugins::host {

/// The frame a plugin cursor `who` (e.g. "provider 'a.b' next") handed back,
/// checked only while DFTRACER_UTILS_PLUGIN_CHECK is on. A broken frame fails
/// the run with an error naming `who` and the rule that broke.
inline dstd::Result<void> check_plugin_frame(const dataframe::DataFrame& frame,
                                             dstd::StringView who) {
    if (!dataframe::plugin_check_enabled()) return {};
    auto ok = dataframe::validate(frame);
    if (ok) return {};
    return common::make_error<"{} returned a broken frame: {}">(
        common::ErrorCode::Internal, who, ok.error().message());
}

/// @ref check_plugin_frame for one column a plugin callback `who` handed back.
inline dstd::Result<void> check_plugin_series(const dataframe::Series& column,
                                              dstd::StringView who) {
    if (!dataframe::plugin_check_enabled()) return {};
    auto ok = dataframe::validate(column);
    if (ok) return {};
    return common::make_error<"{} returned a broken column: {}">(
        common::ErrorCode::Internal, who, ok.error().message());
}

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_FRAME_CHECK_H
