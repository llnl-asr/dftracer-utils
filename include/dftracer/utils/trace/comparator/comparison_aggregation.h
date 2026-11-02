#ifndef DFTRACER_UTILS_TRACE_COMPARATOR_COMPARISON_AGGREGATION_H
#define DFTRACER_UTILS_TRACE_COMPARATOR_COMPARISON_AGGREGATION_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/trace/comparator/comparison_result.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::trace::comparator {

/// What a node query keeps. A set that is not restricted keeps everything and a
/// restricted empty set keeps nothing. The pid and time range cut the events a
/// row is built from, and the category and name sets only select rows.
struct ComparisonFilter {
    bool has_categories = false;
    bool has_names = false;
    bool has_pids = false;
    /// Each sorted and without duplicates. Categories are lower-cased, as the
    /// aggregation compares them.
    dstd::Vector<dstd::String> categories;
    dstd::Vector<dstd::String> names;
    dstd::Vector<std::uint64_t> pids;
    /// Keeps events with ts_begin <= ts < ts_end.
    std::uint64_t ts_begin = 0;
    std::uint64_t ts_end = UINT64_MAX;
    /// Set when the query is more than the above can say (a pattern, `!=`,
    /// `not`, an arg, an `or` across fields): then it is the whole condition,
    /// with every `cat` compared lower-cased, and the sets are unrestricted.
    /// Only an event can answer it, so its chunks are re-read.
    dstd::Optional<duql::Query> expr;

    bool keeps_nothing() const noexcept {
        if (expr) return false;
        return (has_categories && categories.empty()) ||
               (has_names && names.empty()) || (has_pids && pids.empty()) ||
               ts_begin >= ts_end;
    }
    /// Whether a row of `cat` (lower-cased) and `name` passes the sets.
    bool keeps(dstd::StringView cat, dstd::StringView name) const;
    /// Whether `other` cuts the events the same way.
    bool same_cut(const ComparisonFilter& other) const;
};

/// A conjunction of `cat`, `name` and `pid` equalities and `in` lists, of `ts`
/// comparisons against integers and of `or` between sets of one field becomes
/// sets and a range. Any other query becomes `expr`. Fails with Internal when
/// the rewritten query does not parse.
common::Result<ComparisonFilter> filter_from_query(const duql::Query& query);

/// One side of the comparison, collapsed once per filter.
struct SideAggregate {
    /// In the order of the filters.
    dstd::Vector<CollapsedMap> groups;
    /// Of the first filter.
    TraceMetadata meta;
    /// Chunk reads that the queries needed, summed over the cuts. Only the
    /// per-window peaks read, and every chunk when the node has an expression
    /// filter.
    std::size_t metadata_chunks_reread = 0;
    std::size_t group_chunks_reread = 0;
};

/// Answers every filter from the aggregation tier of the indexes of `files`,
/// re-reading the chunks a partial cannot answer (see `group_chunks_reread`).
/// Memory is the rows of the per-window query for one cut at a time, then the
/// collapsed groups.
coro::CoroTask<common::Result<SideAggregate>> aggregate_side(
    dstd::Span<const std::string> files, dstd::StringView index_dir,
    const dstd::Vector<ComparisonFilter>& filters, std::uint64_t window_us,
    std::size_t workers, dstd::StringIntern& strings);

}  // namespace dftracer::utils::trace::comparator

#endif  // DFTRACER_UTILS_TRACE_COMPARATOR_COMPARISON_AGGREGATION_H
