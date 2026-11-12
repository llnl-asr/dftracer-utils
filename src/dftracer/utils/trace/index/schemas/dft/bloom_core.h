// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_BLOOM_CORE_H
#define DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_BLOOM_CORE_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/numbers.h>
#include <dftracer/utils/trace/index/build/chunk_indexer.h>
#include <dftracer/utils/trace/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/trace/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/trace/index/schemas/dft/chunk_statistics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftracer::utils::trace::index {
class Snapshot;
class FileRef;
class FileBuild;
}  // namespace dftracer::utils::trace::index

namespace dftracer::utils::trace::index::store {
enum class ColumnType : std::uint8_t;
}  // namespace dftracer::utils::trace::index::store

namespace dftracer::utils::trace::index::schemas::dft {

/// The bloom/stats/dimension index-build core: per-chunk state plus the harvest
/// and persist seams. Stateless (only static methods and nested types); the
/// caller owns the ChunkState buffer. BloomFold drives these from the POD scan.
class BloomCore {
   public:
    using ChunkStatistics = trace::index::schemas::dft::ChunkStatistics;
    using ChunkDimensionStats = trace::index::extensions::ChunkDimensionStats;
    using ChunkIndexerConfig = trace::index::build::ChunkIndexerConfig;

    /// Fixed bloom filter slots. Indices match DEFAULT_BLOOM_DIMENSIONS
    /// order: name, cat, pid, tid, hhash, fhash, shash.
    enum FixedBloom : std::uint8_t {
        BfName = 0,
        BfCat,
        BfPid,
        BfTid,
        BfHhash,
        BfFhash,
        BfShash,
        BfCount
    };

    /// Fixed dimension_stats slots. Superset of bloom dims plus pid_tid,
    /// ts, dur (which are observed for range stats but not hashed).
    enum FixedDim : std::uint8_t {
        FdName = 0,
        FdCat,
        FdPid,
        FdTid,
        FdPidTid,
        FdHhash,
        FdFhash,
        FdShash,
        FdTs,
        FdDur,
        FdCount
    };

    /// The paths the fixed dimensions are stored under, by FixedDim.
    static dstd::Span<const dstd::StringView> fixed_dimension_names();

    struct ChunkState {
        std::array<trace::index::extensions::ScalableBloomFilter, BfCount>
            fixed_blooms;
        std::array<ChunkDimensionStats, FdCount> fixed_dim_stats;
        /// Blooms of the leading extra dimensions, observed value by value.
        dstd::Vector<trace::index::extensions::ScalableBloomFilter>
            extra_blooms;
        dstd::Vector<ChunkDimensionStats> extra_dim_stats;
        /// The values of an extra dimension past extra_blooms, sorted and
        /// borrowed; write_chunk sizes its bloom for them.
        dstd::Vector<dstd::Vector<dstd::StringView>> extra_values;
        /// Nonzero where extra dimension `e` keeps no bloom for this chunk,
        /// so a probe answers "may match"; the file then keeps none for it.
        dstd::Vector<std::uint8_t> extra_bloom_skip;
        ChunkStatistics statistics;
        trace::index::store::ChunkMetadata metadata;
        std::size_t events_processed = 0;

        ChunkState();
    };

    /// Remembers the keys observe_data has seen, so a repeat needs no hashing,
    /// string compare or bloom probe. It belongs to one chunk's state. Call
    /// clear() before using it on another, or after the state moves.
    class EventCache {
       public:
        static constexpr std::size_t SLOTS = 256;
        static constexpr std::uint32_t NO_SLOT = ChunkDimensionStats::NO_SLOT;

        void clear() {
            if (++epoch_ == 0) {
                names_ = {};
                pids_ = {};
                epoch_ = 1;
            }
        }

       private:
        friend class BloomCore;

        struct NameSlot {
            std::uint64_t key = 0;
            std::uint32_t epoch = 0;
            std::uint32_t name_dim = NO_SLOT;
            std::uint32_t cat_dim = NO_SLOT;
            bool name_in_bloom = false;
            bool cat_in_bloom = false;
            ChunkStatistics::NameSlots stats;
        };
        struct PidSlot {
            std::uint64_t pid = 0;
            std::uint64_t tid = 0;
            std::uint32_t epoch = 0;
            std::uint32_t pid_dim = NO_SLOT;
            std::uint32_t tid_dim = NO_SLOT;
            std::uint32_t pid_tid_dim = NO_SLOT;
            bool pid_in_bloom = false;
            bool tid_in_bloom = false;
            std::uint8_t pid_size = 0;
            std::uint8_t tid_size = 0;
            char pid_buf[24] = {};
            char tid_buf[24] = {};
            ChunkStatistics::PidSlots stats;
        };

        NameSlot& name_slot(std::uint32_t name_id, std::uint32_t cat_id);
        PidSlot& pid_slot(std::uint64_t pid, std::uint64_t tid);

        std::uint32_t epoch_ = 1;
        std::array<NameSlot, SLOTS> names_{};
        std::array<PidSlot, SLOTS> pids_{};
    };

    /// The event harvest. The caller extracts the fields from its own source
    /// into an already-initialized chunk (init_chunk_state) and harvests
    /// columns and extra dimensions itself, since their enumeration is
    /// source-specific.
    static void init_chunk_state(ChunkState& chunk,
                                 const ChunkIndexerConfig& config,
                                 const dstd::Vector<dstd::String>& extra_dims);
    static void observe_data(ChunkState& chunk, EventCache& cache,
                             std::uint32_t name_id, std::uint32_t cat_id,
                             dstd::StringView name, dstd::StringView cat,
                             std::uint64_t pid, std::uint64_t tid,
                             std::uint64_t ts, std::uint64_t dur, bool has_dur,
                             dstd::StringView hhash, dstd::StringView fhash,
                             dstd::StringView shash);
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
                              dstd::StringView value);

    /// The parsed min and max text of a dimension that one caller alone
    /// observes. `valid` is false until they are parsed, and must be reset
    /// when anything else writes the dimension's bounds.
    struct NumericBounds {
        dstd::Optional<duql::Number> min;
        dstd::Optional<duql::Number> max;
        bool valid = false;
    };
    /// As observe_value, with the same min and max text, but a value that
    /// moves no bound is compared as a number and never formatted.
    static void observe_value(ChunkDimensionStats& stats, NumericBounds& bounds,
                              std::int64_t value);
    static void observe_value(ChunkDimensionStats& stats, NumericBounds& bounds,
                              std::uint64_t value);
    static void observe_value(ChunkDimensionStats& stats, NumericBounds& bounds,
                              double value);
    /// observe_extra's min/max and type rule, without the bloom.
    static void observe_value(ChunkDimensionStats& stats, std::int64_t value);
    static void observe_value(ChunkDimensionStats& stats, std::uint64_t value);
    static void observe_value(ChunkDimensionStats& stats, double value);
    static void observe_value(ChunkDimensionStats& stats,
                              dstd::StringView value);

    /// Merge another chunk slice's stats of one dimension into `dst`.
    static void merge_dimension_stats(ChunkDimensionStats& dst,
                                      ChunkDimensionStats& src);

    /// One metadata record named `record_name`: counted and its name kept.
    /// observe_metadata_path records each of its fields.
    static void observe_metadata(ChunkState& chunk,
                                 dstd::StringView record_name);
    static void observe_metadata_path(ChunkState& chunk, dstd::StringView path);

    /// Merge one checkpoint's harvested state into another (fixed/extra blooms,
    /// dim_stats, statistics, metadata counts). `src` is
    /// consumed.
    static void merge_chunk_state(ChunkState& dst, ChunkState& src);

    /// The extensions a tier write replaces: those of `config.extensions` that
    /// are pruning extensions, dft.stats and core.catalog.
    static dstd::Vector<trace::index::store::IndexExtension> tier_extensions(
        const ChunkIndexerConfig& config);

    /// A file's path catalog; the paths are borrowed.
    using Catalog = dstd::Vector<
        dstd::Pair<dstd::StringView, trace::index::store::PathStat>>;

    /// The file-level records a file's chunks add up to: file blooms, file
    /// statistics and the chunk count. write_chunk feeds it; finish_file
    /// writes it.
    class FileAccumulator;
    /// Loads into `acc` what the file-wide records of `file` hold, so new
    /// chunks add to them. Returns the extra dimensions the file is indexed on
    /// besides config.extra_dimensions. A dimension without a file bloom keeps
    /// none, since a filter of the new values alone would miss the old ones.
    static dstd::Result<dstd::Vector<dstd::String>> seed_accumulator(
        const trace::index::Snapshot& snapshot,
        const trace::index::FileRef& file, const ChunkIndexerConfig& config,
        FileAccumulator& acc);

    class FileAccumulator {
       public:
        explicit FileAccumulator(const ChunkIndexerConfig& config);

        struct ExtraBloom {
            trace::index::extensions::ScalableBloomFilter bloom;
            /// A chunk kept no bloom for the dimension, so the file has none.
            bool skip = false;
        };
        ExtraBloom& extra(dstd::StringView dim);

        ChunkStatistics statistics;
        dstd::Vector<trace::index::extensions::ScalableBloomFilter>
            fixed_blooms;
        dftracer::utils::dstd::StringViewMap<ExtraBloom> extras;
        std::uint64_t chunks = 0;
        dstd::Vector<unsigned char> blob;

       private:
        const ChunkIndexerConfig* config_;
    };

    /// Writes one chunk's records for the extensions in `config` and adds it
    /// to `acc`. `extra_dims` names the chunk's extra blooms and statistics
    /// in order. An extra dimension with no value in the chunk gets a zone
    /// with `present = 0` and no bloom.
    static void write_chunk(trace::index::FileBuild& build,
                            std::uint64_t checkpoint_idx,
                            const ChunkState& chunk,
                            const dstd::Vector<dstd::String>& extra_dims,
                            const ChunkIndexerConfig& config,
                            FileAccumulator& acc);

    /// The records a chunk of `observed` data events has for an extra
    /// dimension it never observed, as write_chunk writes them.
    static void write_absent_extra(trace::index::FileBuild& build,
                                   std::uint64_t checkpoint_idx,
                                   dstd::StringView dim, std::uint64_t observed,
                                   const ChunkIndexerConfig& config);

    /// Writes the file-level records, the catalog and the manifest entries
    /// of tier_extensions. `extra_dims` are the dimensions the file keeps.
    /// Returns the file statistics.
    static ChunkStatistics finish_file(
        trace::index::FileBuild& build, FileAccumulator& acc,
        const ChunkIndexerConfig& config,
        const dstd::Vector<dstd::String>& extra_dims, const Catalog& catalog);
};

}  // namespace dftracer::utils::trace::index::schemas::dft

#endif  // DFTRACER_UTILS_TRACE_INDEX_SCHEMAS_DFT_BLOOM_CORE_H
