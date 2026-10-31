#ifndef DFTRACER_UTILS_TRACE_VIEWS_RESULT_JOIN_H
#define DFTRACER_UTILS_TRACE_VIEWS_RESULT_JOIN_H

#include <dftracer/utils/dataframe/dataframe.h>

#include <cstdint>

/// Native equi join over two aggregation-result Batches. A join only COMPARES
/// the leading group-key columns and COPIES the value columns unread, so any
/// aggregation schema rides through opaquely. No Arrow dependency: it composes
/// vec's take() (which fills nulls for OUTER rows via a negative index).
namespace dftracer::utils::trace::views {

enum class JoinType { Inner, Left, Right, Full, LeftSemi, LeftAnti };

/// Equi join `left` and `right` on their first `n_key` columns (the group key).
/// Output columns: the key columns (kept by name), then each non-key left
/// column prefixed "l_", then (unless LEFT_SEMI/LEFT_ANTI) each non-key right
/// column prefixed "r_". An OUTER row nulls the absent side. Rows are sorted by
/// the key for a deterministic result. Returns an empty Batch when the two key
/// schemas (the first `n_key` column names) differ.
dftracer::utils::dataframe::DataFrame join_batches(
    const dftracer::utils::dataframe::DataFrame& left,
    const dftracer::utils::dataframe::DataFrame& right, std::int64_t n_key,
    JoinType type);

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_RESULT_JOIN_H
