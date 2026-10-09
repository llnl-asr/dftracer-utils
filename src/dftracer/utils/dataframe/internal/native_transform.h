#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_NATIVE_TRANSFORM_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_NATIVE_TRANSFORM_H

#include <dftracer/utils/dataframe/lazyframe.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::dataframe {

/// The group-wise transform `kind` over `plan`, grouped by `keys`, as one
/// native plan step instead of a window, a sort by row number and a
/// projection: cumsum, cummax, cummin, cumprod, cumcount, shift (a
/// non-negative `n` up to 1024), diff and ffill over integer and float
/// columns. `typed` is the schema the composed plan declares; the step returns
/// those columns, in the input row order. Empty when the kind, a column type or
/// a column name is not covered, and the caller then composes the plan.
std::optional<LazyFrame> native_group_transform(
    const LazyFrame& plan, const std::vector<std::string>& keys,
    GroupwiseOp kind, std::int64_t n, const Schema& typed);

/// The window plan of the same transform: what the native step must equal.
LazyFrame composed_group_transform(const LazyFrame& plan,
                                   const std::vector<std::string>& keys,
                                   GroupwiseOp kind, std::int64_t n,
                                   RankMethod method, bool ascending);

/// The rows the native transform has grouped on one numeric key column with no
/// key string, since the process started. For tests: a plan that should take
/// that path raises it by its row count.
std::uint64_t native_transform_single_key_rows();

/// The morsels the streaming column op and the streaming with_column have run
/// since the process started. For tests: a plan that should take that path
/// raises it by its morsel count.
std::uint64_t column_scan_morsels();

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_NATIVE_TRANSFORM_H
