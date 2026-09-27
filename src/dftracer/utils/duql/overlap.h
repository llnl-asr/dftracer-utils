#ifndef DFTRACER_UTILS_DUQL_OVERLAP_H
#define DFTRACER_UTILS_DUQL_OVERLAP_H

#include <dftracer/utils/dataframe/series.h>

#include <cstdint>
#include <vector>

namespace dftracer::utils::duql {

/// Each row's matches as CSR lists: row `i` matches the side rows
/// `rows[offsets[i]..offsets[i + 1])`, in ascending side row order.
struct OverlapMatches {
    std::vector<std::int64_t> offsets;
    std::vector<std::int64_t> rows;
};

/// One frame's part of an overlap match: FLAT key, start and duration
/// columns, of equal length. `scale` multiplies the duration into the unit of
/// the start.
struct OverlapColumns {
    std::vector<dataframe::Series> keys;
    dataframe::Series start;
    dataframe::Series duration;
    double scale = 1;
};

/// Matches the rows of `rows` to the rows of `side` with equal keys whose
/// interval `[start, start + duration)` overlaps theirs: `b < a + d` and
/// `a < b + e` for `[a, a + d)` and `[b, b + e)`. A null key, start or
/// duration, a negative duration or a NaN never matches. Times and durations
/// of integer type are compared exactly when both frames hold integers and
/// the scales are 1, else as doubles. Runs in
/// `O((n + m) log m + matches)`. Throws std::invalid_argument on key columns
/// of unequal count or a start or duration that is not a number.
OverlapMatches overlap_matches(const OverlapColumns& rows,
                               const OverlapColumns& side);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_OVERLAP_H
