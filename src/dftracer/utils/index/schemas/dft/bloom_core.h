#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_BLOOM_CORE_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_BLOOM_CORE_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store {
class IndexWrite;
enum class ColumnType : std::uint8_t;
}  // namespace dftracer::utils::index::store

namespace dftracer::utils::index::schemas::dft {

/// The bloom/stats/dimension index-build core: per-chunk state plus the harvest
/// and persist seams. Stateless (only static methods and nested types); the
/// caller owns the ChunkState buffer. BloomFold drives these from the POD scan.
class BloomCore {
   public:
    using ChunkStatistics = index::schemas::dft::ChunkStatistics;
    using ChunkDimensionStats = index::extensions::ChunkDimensionStats;
    using ChunkIndexerConfig = index::build::ChunkIndexerConfig;

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

    /// The paths the fixed dimensions are stored under, by FixedDim.
    static std::span<const std::string_view> fixed_dimension_names();

    struct ChunkState {
        std::array<index::extensions::ScalableBloomFilter, BF_COUNT>
            fixed_blooms;
        std::array<ChunkDimensionStats, FD_COUNT> fixed_dim_stats;
        /// Blooms of the leading extra dimensions, observed value by value.
        std::vector<index::extensions::ScalableBloomFilter> extra_blooms;
        std::vector<ChunkDimensionStats> extra_dim_stats;
        /// The values of an extra dimension past extra_blooms, sorted and
        /// borrowed; write_chunk sizes its bloom for them.
        std::vector<std::vector<std::string_view>> extra_values;
        /// Nonzero where extra dimension `e` keeps no bloom for this chunk,
        /// so a probe answers "may match"; the file then keeps none for it.
        std::vector<std::uint8_t> extra_bloom_skip;
        ChunkStatistics statistics;
        index::store::ChunkMetadata metadata;
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
    static void observe_extra(ChunkState& chunk, std::size_t e,
                              std::uint64_t value);
    static void observe_extra(ChunkState& chunk, std::size_t e, double value);
    static void observe_extra(ChunkState& chunk, std::size_t e,
                              std::string_view value);

    /// observe_extra's min/max and type rule, without the bloom.
    static void observe_value(ChunkDimensionStats& stats, std::int64_t value);
    static void observe_value(ChunkDimensionStats& stats, std::uint64_t value);
    static void observe_value(ChunkDimensionStats& stats, double value);
    static void observe_value(ChunkDimensionStats& stats,
                              std::string_view value);

    /// Merge another chunk slice's stats of one dimension into `dst`.
    static void merge_dimension_stats(ChunkDimensionStats& dst,
                                      ChunkDimensionStats& src);

    /// One metadata record named `record_name`: counted and its name kept.
    /// observe_metadata_path records each of its fields.
    static void observe_metadata(ChunkState& chunk,
                                 std::string_view record_name);
    static void observe_metadata_path(ChunkState& chunk, std::string_view path);

    /// Merge one checkpoint's harvested state into another (fixed/extra blooms,
    /// dim_stats, statistics, metadata counts). `src` is
    /// consumed.
    static void merge_chunk_state(ChunkState& dst, ChunkState& src);

    /// The extensions a tier write replaces: those of `config.extensions` that
    /// are pruning extensions, dft.stats and core.catalog.
    static std::vector<index::store::IndexExtension> tier_extensions(
        const ChunkIndexerConfig& config);

    /// A file's path catalog; the paths are borrowed.
    using Catalog =
        std::vector<std::pair<std::string_view, index::store::PathStat>>;

    /// The file-level records a file's chunks add up to: file blooms, file
    /// statistics and the chunk count. write_chunk feeds it; finish_file
    /// writes it.
    class FileAccumulator {
       public:
        explicit FileAccumulator(const ChunkIndexerConfig& config);

        struct ExtraBloom {
            index::extensions::ScalableBloomFilter bloom;
            /// A chunk kept no bloom for the dimension, so the file has none.
            bool skip = false;
        };
        ExtraBloom& extra(std::string_view dim);

        ChunkStatistics statistics;
        std::vector<index::extensions::ScalableBloomFilter> fixed_blooms;
        dftracer::utils::StringViewMap<ExtraBloom> extras;
        std::uint64_t chunks = 0;
        std::vector<unsigned char> blob;

       private:
        const ChunkIndexerConfig* config_;
    };

    /// Writes one chunk's records for the extensions in `config` and adds it
    /// to `acc`. `extra_dims` names the chunk's extra blooms and statistics
    /// in order. An extra dimension with no value in the chunk gets a zone
    /// with `present = 0` and no bloom.
    static void write_chunk(index::store::IndexWrite& w, int file_id,
                            std::uint64_t checkpoint_idx,
                            const ChunkState& chunk,
                            const std::vector<std::string>& extra_dims,
                            const ChunkIndexerConfig& config,
                            FileAccumulator& acc);

    /// The records a chunk of `observed` data events has for an extra
    /// dimension it never observed, as write_chunk writes them.
    static void write_absent_extra(index::store::IndexWrite& w, int file_id,
                                   std::uint64_t checkpoint_idx,
                                   std::string_view dim, std::uint64_t observed,
                                   const ChunkIndexerConfig& config);

    /// Writes the file-level records, the catalog and the manifest entries
    /// of tier_extensions. `extra_dims` are the dimensions the file keeps.
    /// Returns the file statistics.
    static ChunkStatistics finish_file(
        index::store::IndexWrite& w, int file_id, FileAccumulator& acc,
        const ChunkIndexerConfig& config,
        const std::vector<std::string>& extra_dims, const Catalog& catalog);
};

}  // namespace dftracer::utils::index::schemas::dft

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_BLOOM_CORE_H
