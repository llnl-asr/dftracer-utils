#ifndef DFTRACER_UTILS_TRACE_VISITORS_BLOOM_CORE_H
#define DFTRACER_UTILS_TRACE_VISITORS_BLOOM_CORE_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/trace/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/trace/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/trace/indexing/chunk_statistics.h>
#include <dftracer/utils/trace/indexing/scalable_bloom_filter.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::indexer {
class IndexBatchSink;
class IndexDatabaseWriterContext;
enum class ColumnType : std::uint8_t;
}  // namespace dftracer::utils::utilities::indexer

namespace dftracer::utils::trace::visitors {

/// The bloom/stats/dimension index-build core: per-chunk state plus the harvest
/// and persist seams. Stateless (only static methods and nested types); the
/// caller owns the ChunkState buffer. BloomFold drives these from the POD scan.
class BloomCore {
   public:
    using HashResolutions = indexing::HashResolutions;
    using ChunkStatistics = indexing::ChunkStatistics;
    using ChunkDimensionStats = indexing::ChunkDimensionStats;
    using ChunkIndexerConfig = indexing::ChunkIndexerConfig;

    /// Fixed bloom filter slots. Indices match DEFAULT_BLOOM_DIMENSIONS
    /// order: name, cat, pid, tid, hhash, fhash, shash.
    enum FixedBloom : std::uint8_t {
        BF_NAME = 0,
        BF_CAT,
        BF_PID,
        BF_TID,
        BF_HHASH,
        BF_FHASH,
        BF_SHASH,
        BF_COUNT
    };

    /// Fixed dimension_stats slots. Superset of bloom dims plus pid_tid,
    /// ts, dur (which are observed for range stats but not hashed).
    enum FixedDim : std::uint8_t {
        FD_NAME = 0,
        FD_CAT,
        FD_PID,
        FD_TID,
        FD_PID_TID,
        FD_HHASH,
        FD_FHASH,
        FD_SHASH,
        FD_TS,
        FD_DUR,
        FD_COUNT
    };

    struct ChunkState {
        std::array<indexing::ScalableBloomFilter, BF_COUNT> fixed_blooms;
        std::array<ChunkDimensionStats, FD_COUNT> fixed_dim_stats;
        std::vector<indexing::ScalableBloomFilter> extra_blooms;
        std::vector<ChunkDimensionStats> extra_dim_stats;
        /// Nonzero where extra dimension `e` keeps no bloom for this chunk,
        /// so a probe answers "may match"; the file then keeps none for it.
        std::vector<std::uint8_t> extra_bloom_skip;
        ChunkStatistics statistics;
        HashResolutions hash_resolutions;
        std::size_t events_processed = 0;

        ChunkState();
    };

    /// Per-thread cache so pid/tid decimal conversion runs once per distinct
    /// value rather than once per event.
    struct PidTidCache {
        std::uint64_t last_pid = UINT64_MAX;
        std::uint64_t last_tid = UINT64_MAX;
        char pid_buf[24] = {};
        char tid_buf[24] = {};
        std::uint8_t pid_len = 0;
        std::uint8_t tid_len = 0;
    };

    /// The event harvest. The caller extracts the fields from its own source
    /// into an already-initialized chunk (init_chunk_state) and harvests
    /// columns and extra dimensions itself, since their enumeration is
    /// source-specific.
    static void init_chunk_state(ChunkState& chunk,
                                 const ChunkIndexerConfig& config,
                                 const std::vector<std::string>& extra_dims);
    static void observe_data(ChunkState& chunk, PidTidCache& cache,
                             const ChunkIndexerConfig& config,
                             std::string_view name, std::string_view cat,
                             std::uint64_t pid, std::uint64_t tid,
                             std::uint64_t ts, std::uint64_t dur, bool has_dur,
                             std::string_view hhash, std::string_view fhash,
                             std::string_view shash);
    /// One value of extra dimension `e` (init_chunk_state's extra_dims
    /// order): its text joins the bloom and its value the min/max, kept in
    /// the dimension's own type. Integers and doubles share "double"; a
    /// dimension that also holds strings becomes "mixed" and keeps no
    /// min/max, so a range filter never prunes on it.
    static void observe_extra(ChunkState& chunk, std::size_t e,
                              std::int64_t value);
    static void observe_extra(ChunkState& chunk, std::size_t e, double value);
    static void observe_extra(ChunkState& chunk, std::size_t e,
                              std::string_view value);

    /// observe_extra's min/max and type rule, without the bloom.
    static void observe_value(ChunkDimensionStats& stats, std::int64_t value);
    static void observe_value(ChunkDimensionStats& stats, double value);
    static void observe_value(ChunkDimensionStats& stats,
                              std::string_view value);

    /// Merge another chunk slice's stats of one dimension into `dst`.
    static void merge_dimension_stats(ChunkDimensionStats& dst,
                                      ChunkDimensionStats& src);

    /// `record_name` is the metadata record type (HH/FH/SH); no-op for any
    /// other name or when hash_val/resolved is empty.
    static void observe_metadata(ChunkState& chunk,
                                 std::string_view record_name,
                                 std::string_view hash_val,
                                 std::string_view resolved);

    /// Merge one checkpoint's harvested state into another (fixed/extra blooms,
    /// dim_stats, statistics, sub-zonemaps, hash_resolutions). `src` is
    /// consumed.
    static void merge_chunk_state(ChunkState& dst, ChunkState& src);

    /// Write a file's chunk + file-level bloom/stats/dimension records (plus
    /// columns) to a sink, which may be RocksDB-backed or an SST emitter. Skips
    /// the ROOT_* summary refresh (rebuilt separately). Shared by the streaming
    /// index build and BloomFold::write_to_sink.
    static ChunkStatistics persist_bloom_sink(
        utilities::indexer::IndexBatchSink& sink, int file_id,
        const std::vector<ChunkState>& chunks, const ChunkIndexerConfig& config,
        const std::vector<std::string>& extra_dims,
        const dftracer::utils::StringViewMap<utilities::indexer::ColumnType>&
            columns);

    /// As persist_bloom_sink, plus the concrete-only ROOT_* summary refresh.
    /// Returns the accumulated file statistics.
    static ChunkStatistics persist_bloom(
        utilities::indexer::IndexDatabaseWriterContext& db, int file_id,
        const std::vector<ChunkState>& chunks, const ChunkIndexerConfig& config,
        const std::vector<std::string>& extra_dims,
        const dftracer::utils::StringViewMap<utilities::indexer::ColumnType>&
            columns,
        bool refresh_root_summaries = true);
};

}  // namespace dftracer::utils::trace::visitors

#endif  // DFTRACER_UTILS_TRACE_VISITORS_BLOOM_CORE_H
