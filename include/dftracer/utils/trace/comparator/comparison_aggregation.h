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

/// What a node query keeps, in the terms of an aggregation query. A set that
/// is not restricted keeps everything; a restricted empty set keeps nothing.
/// The pid and time range cut the events a row is built from; the category
/// and name sets only select rows.
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

/// The aggregation config the comparator builds its indexes with and queries
/// them with. Percentiles are on, so the partials hold the quantile stores and
/// the quantile query merges them without a re-read.
index::schemas::dft::agg::AggregationConfig comparison_index_config();

/// The filter that `query` stands for. A conjunction of `cat`, `name` and
/// `pid` equalities and `in` lists, of `ts` comparisons against integers, and
/// of `or` between sets of one of those fields becomes sets and a range,
/// answered from the index. Any other query becomes `expr`. A category
/// compares lower-cased on both sides, as the aggregation keys it. Internal
/// when the rewritten query does not parse.
Result<ComparisonFilter> filter_from_query(const duql::Query& query);

/// One side of the comparison, collapsed once per filter.
struct SideAggregate {
    /// In the order of the filters.
    dstd::Vector<CollapsedMap> groups;
    /// Of the first filter.
    TraceMetadata meta;
    /// Chunk reads that the queries needed, summed over the cuts. The
    /// metadata and the quantiles come from the stored partials and read
    /// none. The per-window peaks read only a chunk whose partial holds more
    /// than one pid or tid or crosses a window, and every chunk when the
    /// node has an expression filter.
    std::size_t metadata_chunks_reread = 0;
    std::size_t group_chunks_reread = 0;
};

/// Answers every filter from the aggregation tier of the indexes of `files`
/// (the config the comparator builds them with). Two queries run for each
/// distinct cut of the events: one grouped by pid, tid and
/// `window_us` window for the busiest thread of each window, and one grouped by
/// category and name for the quantiles, the stats and the metadata (the
/// distinct processes, threads, files and pairs). Both are answered from the
/// stored partials, but for the chunks a partial cannot answer (see
/// `group_chunks_reread`). Memory: the rows of the first query for one cut at a
/// time, then the collapsed groups.
coro::CoroTask<Result<SideAggregate>> aggregate_side(
    dstd::Span<const std::string> files, dstd::StringView index_dir,
    const dstd::Vector<ComparisonFilter>& filters, std::uint64_t window_us,
    std::size_t workers, dstd::StringIntern& strings);

}  // namespace dftracer::utils::trace::comparator

#endif  // DFTRACER_UTILS_TRACE_COMPARATOR_COMPARISON_AGGREGATION_H
