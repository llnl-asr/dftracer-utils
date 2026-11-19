// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_RESHAPE_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_RESHAPE_H

#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

/// The number of each row of a set of key columns, and the first row of each
/// number.
struct RowNumbers {
    Series ids;    ///< Int64, one number per row
    Series first;  ///< Int64, the first row that holds each number
};

/// Numbers the distinct rows of `keys` (equal length, a null equals a null, a
/// NaN equals a NaN): by the order of first appearance, or with `sorted` by
/// ascending value, nulls first. An InvalidArgument error for a key column of
/// a type with no per-row value.
dstd::Result<RowNumbers> number_rows(dstd::Span<const Series> keys,
                                     bool sorted);

/// For an unstack: the row that fills each cell of the result, column by
/// column (cell `c * row_count + r` is result column `c`, row `r`), or -1 for
/// a cell no row fills. `rows[i]` and `cols[i]` are row `i`'s result row and
/// column, both in range. An InvalidArgument error when two rows fill one
/// cell, or when the result has more than 2^31 - 1 cells.
dstd::Result<Series> unstack_positions(const Series& rows, const Series& cols,
                                       std::int64_t row_count,
                                       std::int64_t col_count);

/// The columns of an unstack: result column `c` of `values` taken at the
/// `row_count` rows `positions` names for it, a null where it names -1.
dstd::Result<dstd::Vector<Series>> unstack_columns(const Series& values,
                                                   const Series& positions,
                                                   std::int64_t row_count);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_RESHAPE_H
