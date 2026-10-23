#ifndef DFTRACER_UTILS_INDEX_BUILD_BATCH_BUILDER_H
#define DFTRACER_UTILS_INDEX_BUILD_BATCH_BUILDER_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/scoped_fd.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_drain.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/runtime/coro/task.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils {
class CoroScope;
}  // namespace dftracer::utils

namespace dftracer::utils::dstd {
class StringIntern;
}  // namespace dftracer::utils::dstd

namespace dftracer::utils::index::schemas::dft::agg {
class AggregationFold;
}  // namespace dftracer::utils::index::schemas::dft::agg

namespace dftracer::utils::index::gzip {
struct GzipMember;
}  // namespace dftracer::utils::index::gzip

namespace dftracer::utils::index::store {
class IndexDatabase;
}  // namespace dftracer::utils::index::store

namespace dftracer::utils::index::build {

using dftracer::utils::CoroScope;

/// Removes the directory of one file's BloomFold spill runs, for a write that
/// failed before it could ingest them.
void remove_spill_runs(
    const std::vector<store::IndexDatabaseSstWriterContext::Artifacts>& runs);

/// Ingests one file's BloomFold spill runs into `db` as one ingest, in run
/// order, then removes their directory, also when the ingest throws.
void ingest_spill_runs(
    store::IndexDatabase& db,
    std::vector<store::IndexDatabaseSstWriterContext::Artifacts> runs);

/// Bytes of memory, reserved capacity included, a sink holds before its worker
/// commits it. Each commit writes one SST per key prefix, so the limit also
/// sets the SST count: on 8,404 small files at 48 workers, 16 MiB left two SSTs
/// per worker and prefix and 32 MiB left one.
inline constexpr std::size_t MAX_SINK_BYTES = std::size_t{32} << 20;

/// The limit a build applies; tests lower it to make small inputs commit.
inline std::atomic<std::size_t> g_max_sink_bytes{MAX_SINK_BYTES};

/// Where the blocks of consecutive files start, then the end: about `workers`
/// blocks of equal cost (equal file counts when `costs` is empty). A worker
/// that writes one block into its own sinks leaves SSTs that overlap no other
/// block's.
dstd::Vector<std::size_t> cut_blocks(std::size_t files,
                                     std::span<const std::uint64_t> costs,
                                     std::size_t workers);

inline constexpr std::array<std::string_view, 7> DEFAULT_BLOOM_DIMENSIONS = {
    "name", "cat", "pid", "tid", "hhash", "fhash", "shash",
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
    index::build::ChunkIndexerConfig bloom_config;
    std::vector<std::string> bloom_dimensions;
    /// Write only the tier extensions of bloom_config.extensions (none when
    /// build_bloom is off) and, when agg_fold_factory is set, the aggregation
    /// tier, for files whose members are current; members, the file record
    /// and hash tables are left as they are.
    bool tier_only = false;
    /// Bytes the build may hold at once: 0 resolves through
    /// memory::resolve_spill_budget, memory::NO_SPILL_BUDGET leaves the build
    /// unbounded. Parse workers admit a file only while its estimated bytes
    /// fit.
    std::uint64_t memory_budget = 0;
    /// RecordSchema id every file is decoded with; empty detects each file's.
    std::string schema;

    /// If > 0, process files in sub-batches of this size, flushing parsed
    /// artifacts to the write phase between sub-batches. Bounds peak memory
    /// to ~flush_every_files worth of ParsedBloomJob state. 0 = no flush
    /// (all files parsed before any write).
    std::size_t flush_every_files = 0;

    /// Factory for the aggregation-tier fold, created once per file with the
    /// fused-scan intern that produced its events and the file's id. When set,
    /// the batch build steps an AggregationFold in the same single parse as
    /// bloom/dict and its per-file out-of-band outputs land in
    /// IndexBuildBatchResult::agg_outputs.
    using AggFoldFactory = std::function<
        std::unique_ptr<index::schemas::dft::agg::AggregationFold>(
            dftracer::utils::dstd::StringIntern& build_intern, int file_id)>;
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
    /// With preassigned ids: the ids from this one up were reserved by this
    /// build, so the index holds no data for them and their tiers need no
    /// clearing. The default marks none.
    int first_fresh_file_id = std::numeric_limits<int>::max();

    /// If non-empty, parallel to `file_paths`: an open read-only descriptor
    /// of each file, which the build uses instead of opening the file. The
    /// build takes each one out of this vector when it prepares the file, and
    /// closes it as soon as that file is parsed. A null entry opens the file
    /// as usual.
    std::vector<dstd::SharedPtr<ScopedFd>> input_fds;

    /// If non-empty, parallel to `file_paths`: the relative work of each file,
    /// such as its size in bytes. A worker takes a run of consecutive files,
    /// and the runs are cut to hold about equal work. Empty cuts them to
    /// equal file counts.
    std::vector<std::uint64_t> file_costs;

    /// Optional per-file member slice (cross-rank file splitting). When
    /// non-empty, must be parallel to `file_paths`. A null/empty entry
    /// means "process the whole file"; a populated entry restricts the
    /// build to `[member_begin, member_end)`. The `members` vector must
    /// outlive the batch (typically stored in a shared member map).
    struct FileSlice {
        const std::vector<index::gzip::GzipMember>* members = nullptr;
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

    /// Optional write factory. If set, the write phase constructs a fresh
    /// IndexWrite per batch via this factory instead of opening the
    /// RocksDB-backed writer on `index_dir`. Used by the distributed (SST)
    /// pipeline to route writes to per-worker SstWriterContext instances.
    /// `sink_commit` must also be set and is responsible for finalising
    /// each write (RocksDB path: call .commit(); SST path: flush + route
    /// Artifacts to a registry).
    using SinkFactory =
        std::function<std::unique_ptr<index::store::IndexWrite>()>;
    using SinkCommitFn = std::function<void(index::store::IndexWrite&)>;
    SinkFactory sink_factory;
    SinkCommitFn sink_commit;

    /// Commits a file whose build spilled: its runs, which must be ingested
    /// in this order in one ingest, and are owned by the callee. Required
    /// with sink_factory for files to spill; without a sink the builder
    /// ingests them itself.
    using SpillCommitFn = std::function<void(
        std::vector<index::store::IndexDatabaseSstWriterContext::Artifacts>)>;
    SpillCommitFn spill_commit;
    /// Where spill runs go; empty puts them beside the index as
    /// "<index>.spill".
    std::string spill_dir;
};

struct IndexBuildBatchMetrics {
    std::uint64_t parse_ns = 0;
    std::uint64_t write_ns = 0;
    std::size_t files_enqueued = 0;
    std::size_t files_parsed = 0;
    std::size_t files_written = 0;
    /// Most files parsed at the same time under the memory budget.
    std::size_t max_files_in_flight = 0;
    /// Files whose build state passed their share and spilled to disk.
    std::size_t files_spilled = 0;
    /// The most spill runs one file wrote while it was parsed.
    std::size_t max_spill_runs = 0;
};

struct IndexBuildBatchResult {
    std::vector<IndexBuildResult> results;
    std::size_t indexed = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;
    std::uint64_t total_events = 0;
    IndexBuildBatchMetrics metrics;

    /// Per-file aggregation-fold out-of-band outputs (tracker, time bounds),
    /// for callers using agg_fold_factory. Drained via
    /// aggregators::merge_aggregation_folds.
    std::vector<index::schemas::dft::agg::AggFoldOutput> agg_outputs;
};

class BatchBuilder {
   public:
    static coro::CoroTask<IndexBuildBatchResult> process(
        CoroScope* scope, std::shared_ptr<IndexBuildBatchConfig> config);
};

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_BATCH_BUILDER_H
