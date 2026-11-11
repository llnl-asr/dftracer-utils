// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_QUERY_OVERLAP_H
#define DFTRACER_UTILS_DATAFRAME_QUERY_OVERLAP_H

#include <dftracer/utils/common/test_probes.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#if DFTRACER_UTILS_TEST_PROBES
#include <atomic>
#endif

namespace dftracer::utils::dataframe::query {

#if DFTRACER_UTILS_TEST_PROBES
/// Tree nodes and side rows that @ref overlap_matches visited. For tests.
inline std::atomic<std::uint64_t> g_overlap_steps{0};
#endif

/// Each row's matches as CSR lists: row `i` matches the side rows
/// `rows[offsets[i]..offsets[i + 1])`, in ascending side row order.
struct OverlapMatches {
    dstd::Vector<std::int64_t> offsets;
    dstd::Vector<std::int64_t> rows;
};

/// One frame's part of an overlap match: FLAT key, start and duration
/// columns, of equal length. `scale` multiplies the duration into the unit of
/// the start.
struct OverlapColumns {
    dstd::Vector<dataframe::Series> keys;
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
/// `O((n + m) log m + matches)`. A start or duration that is not a number is an
/// InvalidArgument error. Key columns of unequal count, or a start and
/// duration of unequal length, are fatal.
dstd::Result<OverlapMatches> overlap_matches(const OverlapColumns& rows,
                                             const OverlapColumns& side);

}  // namespace dftracer::utils::dataframe::query

#endif  // DFTRACER_UTILS_DATAFRAME_QUERY_OVERLAP_H
