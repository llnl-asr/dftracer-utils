// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_COMPARATOR_COMPARISON_COLLAPSE_H
#define DFTRACER_UTILS_TRACE_COMPARATOR_COMPARISON_COLLAPSE_H

#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/comparator/comparison_aggregation.h>
#include <dftracer/utils/trace/comparator/comparison_result.h>

#include <cstddef>

namespace dftracer::utils::trace::comparator {

/// The trace metadata of the rows of `merged` that `filter` keeps. `file_count`
/// stands in for the distinct files when the trace carries no file hash.
TraceMetadata extract_metadata(const AggQueryResult& merged,
                               const ComparisonFilter& filter,
                               std::size_t file_count);

/// Collapses the rows of `filter` to one entry per (category, operation). In a
/// window the largest value over the (pid, tid) pairs of `windows` counts, and
/// across windows the mean of those. `merged` holds the quantile stores. Two
/// sides collapsed against one `dst` are comparable.
CollapsedMap collapse_by_group(const AggQueryResult& windows,
                               const AggQueryResult& merged,
                               const ComparisonFilter& filter,
                               dstd::StringIntern& dst);

}  // namespace dftracer::utils::trace::comparator

#endif  // DFTRACER_UTILS_TRACE_COMPARATOR_COMPARISON_COLLAPSE_H
