// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_FIELD_STAT_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_FIELD_STAT_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/field_stat.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

/// SIMD one-pass reduction of a contiguous column range into a FieldStat (power
/// sums + min/max, plus exact integer sum/min/max for integer domains). `end <
/// 0` means the whole column. Equivalent, value for value, to folding the range
/// through FieldStat::add; the fast path (dense Flat Float64/Int64) vectorizes,
/// everything else falls back to the scalar accumulation. This is the batch
/// path for the atom; grouped aggregation still scatters one value per row.
FieldStat field_stat_reduce(const Series& col, std::int64_t begin = 0,
                            std::int64_t end = -1);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_FIELD_STAT_H
