// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_AGG_COMPUTED_H
#define DFTRACER_UTILS_DATAFRAME_AGG_COMPUTED_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

namespace dftracer::utils::dataframe {

/// Value columns of a group-by that are expressions: `values[i]` is null
/// exactly where `roots[i]` is valid, and the roots read `inputs`, which have
/// the keys' rows. The drivers that fold rows in windows evaluate the roots a
/// window at a time; the others evaluate them over all rows first.
struct AggComputed {
    dstd::Span<const Expr> roots;
    dstd::Span<const Series* const> inputs;
};

/// @ref group_agg over `values` some of which @p computed supplies.
dstd::Result<DataFrame> group_agg_computed(
    dstd::Span<const Series* const> keys,
    dstd::Span<const Series* const> values, dstd::Vector<AggSpec> specs,
    dstd::Span<const dstd::String> key_names, const AggComputed& computed);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_AGG_COMPUTED_H
