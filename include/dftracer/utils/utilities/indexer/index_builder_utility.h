#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_BUILDER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_BUILDER_UTILITY_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/trace/aggregators/aggregation_drain.h>
#include <dftracer/utils/trace/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils {
class CoroScope;
class StringIntern;
}  // namespace dftracer::utils

namespace dftracer::utils::trace::views::detail {
class AggregationFold;
}  // namespace dftracer::utils::trace::views::detail

namespace dftracer::utils::utilities::indexer::internal {
struct GzipMember;
}  // namespace dftracer::utils::utilities::indexer::internal

namespace dftracer::utils::utilities::indexer {

using dftracer::utils::CoroScope;

inline constexpr std::array<std::string_view, 7> DEFAULT_BLOOM_DIMENSIONS = {
    "name", "cat", "pid", "tid", "hhash", "fhash", "shash",
};

inline constexpr std::array<std::string_view, 5> DEFAULT_EXTRA_DIMENSIONS = {
    "ret", "count", "offset", "epoch", "step",
};

struct IndexBuildResult {
    std::string file_path;
    std::string index_path;
    bool success = false;
    bool was_skipped = false;
    bool index_created = false;
    std::size_t events_processed = 0;
    std::size_t chunks_processed = 0;
    std::size_t total_lines = 0;
    std::string error_message;
};

struct IndexBuildBatchConfig {
    std::vector<std::string> file_paths;
    std::string index_dir;
    std::size_t checkpoint_size = constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    std::size_t parallelism = 1;
    bool force_rebuild = false;
    /// Build the bloom/stats/dimension tier. Off skips the bloom tier, which is
    /// the biggest per-event cost; only the raw-trace query path reads it, so
    /// an aggregation-only index (dfanalyzer) does not need it.
    bool build_bloom = true;
    trace::indexing::ChunkIndexerConfig bloom_config;
    std::vector<std::string> bloom_dimensions;
    bool rebuild_root_summaries = true;

    /// If > 0, process files in sub-batches of this size, flushing parsed
    /// artifacts to the write phase between sub-batches. Bounds peak memory
    /// to ~flush_every_files worth of ParsedBloomJob state. 0 = no flush
    /// (all files parsed before any write).
    std::size_t flush_every_files = 0;

    /// Factory for the aggregation-tier fold, created once per file with the
    /// fused-scan intern that produced its events. When set, the batch build
    /// steps an AggregationFold in the same single parse as bloom/dict and its
    /// per-file out-of-band outputs land in IndexBuildBatchResult::agg_outputs.
    using AggFoldFactory =
        std::function<std::unique_ptr<trace::views::detail::AggregationFold>(
            dftracer::utils::StringIntern& build_intern)>;
    AggFoldFactory agg_fold_factory;

    /// Optional callback with (files_done, total_files) as files finish
    /// parsing. Called from worker threads (throttled); must be thread-safe.
    using ProgressFn = std::function<void(std::size_t done, std::size_t total)>;
    ProgressFn progress;

    /// If non-empty, parallel to `file_paths`: use these file_ids instead
    /// of allocating via `get_or_create_file_info`. Used by the distributed
    /// indexer where the coordinator pre-registers all files. When set,
    /// the write phase skips the DEFAULT-CF registry open/write step.
    std::vector<int> preassigned_file_ids;

    /// Optional per-file member slice (cross-rank file splitting). When
    /// non-empty, must be parallel to `file_paths`. A null/empty entry
    /// means "process the whole file"; a populated entry restricts the
    /// build to `[member_begin, member_end)`. The `members` vector must
    /// outlive the batch (typically stored in a shared member map).
    struct FileSlice {
        const std::vector<internal::GzipMember>* members = nullptr;
        std::size_t member_begin = 0;
        std::size_t member_end = 0;
        /// When true, this file's file-scoped data (member table,
        /// bloom/manifest/hashtable, file_metadata) is NOT persisted by
        /// the write phase. Aggregation/system-metrics SSTs produced by
        /// extra visitors are still collected. Set by the MPI driver for
        /// sliced ranks where `member_begin > 0` to avoid cross-rank key
        /// collisions on file-scoped CFs.
        bool skip_file_scoped_writes = false;
    };
    std::vector<FileSlice> file_slices;

    /// Optional batch-sink factory. If set, the write phase constructs a
    /// fresh sink per batch via this factory instead of opening the
    /// RocksDB-backed writer on `index_dir`. Used by the distributed (SST)
    /// pipeline to route writes to per-worker SstWriterContext instances.
    /// `sink_commit` must also be set and is responsible for finalising
    /// each sink (RocksDB path: call .commit(); SST path: flush + route
    /// Artifacts to a registry).
    using SinkFactory = std::function<std::unique_ptr<IndexBatchSink>()>;
    using SinkCommitFn = std::function<void(IndexBatchSink&)>;
    SinkFactory sink_factory;
    SinkCommitFn sink_commit;
};

struct IndexBuildBatchMetrics {
    std::uint64_t parse_ns = 0;
    std::uint64_t write_ns = 0;
    std::size_t files_enqueued = 0;
    std::size_t files_parsed = 0;
    std::size_t files_written = 0;
};

struct IndexBuildBatchResult {
    std::vector<IndexBuildResult> results;
    std::size_t indexed = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;
    std::uint64_t total_events = 0;
    IndexBuildBatchMetrics metrics;

    /// Per-file aggregation-fold out-of-band outputs (observed keys, tracker,
    /// time bounds), for callers using agg_fold_factory. Drained via
    /// aggregators::merge_aggregation_folds.
    std::vector<trace::aggregators::AggFoldOutput> agg_outputs;
};

class IndexBatchBuilderUtility {
   public:
    static coro::CoroTask<IndexBuildBatchResult> process(
        CoroScope* scope, std::shared_ptr<IndexBuildBatchConfig> config);
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_BUILDER_UTILITY_H
