// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_FRAME_NATIVE_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_FRAME_NATIVE_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>

#include <optional>

namespace dftracer::utils::dataframe {

/// `f` as bytes: a magic and version, the row count, and each column's name
/// and buffers (the spill format, so a column keeps its type, nulls, JSON
/// flag, time unit, time zone, decimal and fixed size, and a list or struct
/// column its children). Machine byte order: for stores that stay on one
/// host.
dstd::String frame_to_native(const DataFrame& f);

/// The frame `frame_to_native` wrote, or nullopt when `bytes` is another
/// format or version, is truncated, or holds a column that does not decode.
/// Never throws.
dstd::Optional<DataFrame> frame_from_native(dstd::StringView bytes) noexcept;

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_FRAME_NATIVE_H
