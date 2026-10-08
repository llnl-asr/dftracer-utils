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

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_NATIVE_TRANSFORM_H
