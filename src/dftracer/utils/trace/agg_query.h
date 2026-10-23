#ifndef DFTRACER_UTILS_TRACE_AGG_QUERY_H
#define DFTRACER_UTILS_TRACE_AGG_QUERY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/runtime/coro/task.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

// One query API over the chunk partials of the aggregation tier. A query
// names the files, a group-by, an optional time window and filters, and gets
// one row per group key, each an AggPartial that has the stored partials
// merged into it as the iterator over RocksDB yields them.
//
// A stored partial is added whole when it answers the query exactly. It is
// answered by re-reading its chunk when it cannot:
//   - a window: the events of the partial do not all start in one window,
//     that is the windows of its first and of its last event start differ;
//   - the time range or the pid filter cuts the partial;
//   - the group-by names a pid or tid and the partial holds more than one,
//     or names the file or an extra key (partials do not hold them);
//   - the group-by splits events from profiles and the partial holds both;
//   - the query has an event filter, which only an event can answer;
//   - quantiles are asked for and the partial has per-event values but
//     stores no quantiles.
// A file that has no usable partials (its tier was written without the
// fold, or with other params) is re-read whole, in the chunks the time range
// leaves. A query that sets `reread` false reads nothing but partials: what a
// re-read would have answered is counted in the stats and left out.
//
// An event belongs to the window of its ts, as in compute_time_bucket.
// A chunk is the lines that END in its gzip member, the rule of the index
// fold that builds the partials, the bloom filters and the chunk statistics.
// A re-read follows it, so a member cut inside a line neither doubles nor
// drops that line.

namespace dftracer::utils::trace {

struct AggQuerySource {
    /// The index store of the files.
    dstd::String index_path;
    /// Trace paths as the index was built from them.
    dstd::Vector<dstd::String> files;
    /// The params hash the files of this index were built with. Unset takes
    /// config.params_hash(); a caller that does not know the build config
    /// reads the hash off the index.
    dstd::Optional<std::uint64_t> params_hash;
};

struct AggQuery {
    dstd::Vector<AggQuerySource> sources;

    /// The config the indexes were built with. Its params hash decides
    /// whether a file's partials are usable, its custom metric fields and
    /// track_default_args shape the per-arg stats of a re-read, its
    /// extra_group_keys are the extra keys of the partials (string args), its
    /// time_interval_us is the span of a pre-aggregated counter, and its
    /// sketch_accuracy the accuracy of the quantile stores.
    index::schemas::dft::agg::AggregationConfig config;

    bool by_pid = false;
    bool by_tid = false;
    bool by_file = false;
    /// Groups by the hhash text of the events; a partial keeps one host.
    bool by_host = false;
    /// Splits events (ph other than C), profiles (counters of a category
    /// other than sys) and the pre-aggregated sys counters into separate rows.
    bool by_kind = false;
    /// Groups by the values of the build's extra group keys, which
    /// config.extra_group_keys names in the order the build had them.
    bool by_extra = false;

    /// 0 for no windows; else the window length. With config.use_relative_time
    /// windows start at multiples of it from config.reference_timestamp.
    std::uint64_t window_us = 0;

    /// Empty keeps all. Categories compare lower-cased.
    dstd::Vector<dstd::String> categories;
    dstd::Vector<dstd::String> names;
    dstd::Vector<std::uint64_t> pids;
    /// The hhash texts to keep; empty keeps all.
    dstd::Vector<dstd::String> hosts;
    /// Keeps events with ts_begin <= ts < ts_end.
    std::uint64_t ts_begin = 0;
    std::uint64_t ts_end = UINT64_MAX;

    /// Keeps the events the expression holds for (the whole line, with args
    /// fallback). The stored partials cannot answer it, so every chunk is
    /// re-read; the sets above still prune what they can without a read. An
    /// expression on `cat` must compare lower-cased on both sides.
    dstd::Optional<duql::Query> filter;

    /// Tasks that run at once; 0 is the thread count of the executor the
    /// query runs on.
    std::size_t workers = 0;

    /// The quantile stores of the rows; without them none are merged.
    bool quantiles = false;
    /// The per-arg stats of the rows.
    bool args = true;

    /// False reads no chunk: a partial or a file that only a re-read could
    /// answer is counted in AggQueryStats::unanswered and left out.
    bool reread = true;
};

/// What a row holds, when the query splits them (AggQuery::by_kind).
using AggKind = index::schemas::dft::agg::PartialKind;

struct AggKey {
    std::uint32_t cat = 0xFFFFFFFFu;    ///< id in AggQueryResult::text
    std::uint32_t name = 0xFFFFFFFFu;   ///< id in AggQueryResult::text
    std::uint32_t extra = 0xFFFFFFFFu;  ///< id of the extra values joined by
                                        ///< 0x1F, none when all are absent,
                                        ///< by_extra only
    std::uint32_t host = 0xFFFFFFFFu;   ///< id of the hhash text, by_host only
    /// id of the text of a file hash that is not 16 hex digits, by_file only;
    /// such a hash keeps its text, the others are rendered from `file`.
    std::uint32_t file_text = 0xFFFFFFFFu;
    AggKind kind = AggKind::Event;  ///< Event unless by_kind
    std::uint64_t pid = 0;          ///< 0 unless by_pid
    std::uint64_t tid = 0;          ///< 0 unless by_tid
    std::uint64_t file = 0;         ///< file hash, 0 unless by_file or no file
    std::uint64_t window = 0;       ///< window start, 0 without windows

    bool operator==(const AggKey&) const = default;
};

struct AggKeyHash {
    using is_avalanching = void;
    std::size_t operator()(const AggKey& k) const noexcept {
        using dftracer::utils::hash::splitmix64;
        std::uint64_t h = splitmix64(k.window ^ k.extra);
        h = splitmix64(h ^ k.file ^ (std::uint64_t{k.file_text} << 1));
        h = splitmix64(h ^ (std::uint64_t{k.host} << 2 |
                            static_cast<std::uint64_t>(k.kind)));
        h = splitmix64(h ^ k.tid);
        h = splitmix64(h ^ k.pid);
        return static_cast<std::size_t>(
            splitmix64(h ^ (std::uint64_t{k.cat} << 32 | k.name)));
    }
};

struct AggQueryStats {
    std::size_t files = 0;
    /// Files that had no usable partials and were read whole.
    std::size_t files_without_partials = 0;
    std::size_t partials_read = 0;
    /// Added whole.
    std::size_t partials_merged = 0;
    /// Dropped by the category, name, pid or time filter without a read.
    std::size_t partials_pruned = 0;
    /// Answered by a re-read of their chunk.
    std::size_t partials_reread = 0;
    /// Chunk reads; a chunk with several re-read partials is read once.
    std::size_t chunks_reread = 0;
    /// Partials and files that needed a re-read the query did not allow.
    std::size_t unanswered = 0;
    std::uint64_t events_read = 0;
    /// The most events one parse of a re-read decoded into the event buffer.
    std::size_t max_batch_events = 0;
    /// The rows at the end. Rows are never removed, so this is also the most
    /// the result map held at once.
    std::size_t groups = 0;
};

class AggQueryResult {
   public:
    using Rows =
        dstd::HashMap<AggKey, index::schemas::dft::agg::AggPartial, AggKeyHash>;

    AggQueryResult() : strings_(dstd::make_unique<dstd::StringIntern>()) {}

    Rows rows;
    AggQueryStats stats;

    /// The text of a cat, name or extra id, and of an arg id in a row.
    std::string_view text(std::uint32_t id) const {
        return id == dstd::StringIntern::NO_ID ? std::string_view{}
                                               : strings_->resolve(id);
    }
    dstd::StringIntern& strings() { return *strings_; }

   private:
    dstd::UniquePtr<dstd::StringIntern> strings_;
};

/// Runs `query`. Files are read in parallel on the runtime of the caller: up
/// to `workers` tasks run at once, each a file scan or a chunk re-read, each
/// folding into a map of its own that is merged into the result when the task
/// ends. Memory: the result map, plus at most `workers` task maps, each as
/// large as the groups of one file (or one chunk), plus the re-read ids (12
/// bytes per queued partial) of the files being scanned and one parsed batch
/// of events per task. Fails with NotFound for a file that is not in its
/// index, Parse for a malformed partial, and what the reader reports for a
/// chunk.
coro::CoroTask<Result<AggQueryResult>> run_agg_query(const AggQuery& query);

}  // namespace dftracer::utils::trace

#endif  // DFTRACER_UTILS_TRACE_AGG_QUERY_H
