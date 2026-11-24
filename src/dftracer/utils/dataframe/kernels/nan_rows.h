// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_NAN_ROWS_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_NAN_ROWS_H

#include <dftracer/utils/dataframe/series.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

/// The Bool mask, with no nulls, that is false where a Float32 or Float64
/// column `c` holds a NaN (a null row is kept), in one pass. `*nan_count` is
/// the number of NaN rows, so a caller with none returns its input unchanged.
/// An invalid Series for a column of another type, with `*nan_count` 0, and
/// also for a column that is not FLAT and has no NaN, which is not copied.
/// `*nan_count` is -1 when a non-FLAT column cannot be read.
Series keep_non_nan(const Series& c, std::int64_t* nan_count);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_NAN_ROWS_H
