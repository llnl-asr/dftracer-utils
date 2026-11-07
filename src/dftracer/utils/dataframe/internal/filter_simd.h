// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_FILTER_SIMD_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_FILTER_SIMD_H

#include <dftracer/utils/dataframe/abi.h>

#include <cstdint>

struct dftu_series;

namespace dftracer::utils::dataframe {

/// Fill `out` (capacity >= v.length) with the row indices where v > threshold,
/// using a Highway Compress compaction. Applies only to a no-null, 4/8-byte
/// numeric column; returns the count, or -1 when not applicable so the caller
/// falls back to the scalar path.
std::int64_t compact_gt(const dftu_series& v, dftu_scalar threshold,
                        std::int64_t* out);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_FILTER_SIMD_H
