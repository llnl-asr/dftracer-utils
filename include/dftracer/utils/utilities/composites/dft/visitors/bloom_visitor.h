#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VISITORS_BLOOM_VISITOR_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VISITORS_BLOOM_VISITOR_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/utilities/composites/dft/dft_event_visitor.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>

#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::indexer {
class IndexBatchSink;
class IndexDatabaseWriterContext;
}  // namespace dftracer::utils::utilities::indexer

namespace dftracer::utils::utilities::composites::dft::visitors {

class BloomVisitor : public DftEventVisitor {
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
        std::array<indexing::BloomFilter, BF_COUNT> fixed_blooms;
        std::array<ChunkDimensionStats, FD_COUNT> fixed_dim_stats;
        std::vector<indexing::BloomFilter> extra_blooms;
        std::vector<ChunkDimensionStats> extra_dim_stats;
        ChunkStatistics statistics;
        HashResolutions hash_resolutions;
        std::size_t events_processed = 0;

        ChunkState();
    };

    BloomVisitor(ChunkIndexerConfig config,
                 std::vector<std::string> dimensions);
    BloomVisitor(const BloomVisitor&) = delete;
    BloomVisitor& operator=(const BloomVisitor&) = delete;
    BloomVisitor(BloomVisitor&&) noexcept = default;
    BloomVisitor& operator=(BloomVisitor&&) noexcept = default;

    void begin(std::size_t num_checkpoints) override;
    void on_checkpoint(std::size_t checkpoint_idx) override;
    void on_event(const EventRecord& record) override;

    std::unique_ptr<DftEventVisitor> create_parallel_slice() const override;
    void merge_parallel_slice(DftEventVisitor& slice) override;

    void finalize(indexer::IndexDatabaseWriterContext& writer, int file_id);
    /// Emit bloom / stats / dimension records plus name dictionary/postings
    /// to a sink backend. Skips ROOT_* summaries (rebuilt separately by
    /// `IndexDatabase::rebuild_root_summaries()`). Works for both the
    /// RocksDB-backed writer and the SST writer.
    void finalize_sink_only(indexer::IndexBatchSink& sink, int file_id);

    /// Emit per-checkpoint chunk records (bloom, stats, dim_stats,
    /// name_chunk_postings) using the current `chunks_` buffer, merge their
    /// state into the persistent file-level accumulator, then clear
    /// `chunks_` and advance the base index. Used for mid-chunk slice
    /// rotation when `chunks_` would otherwise grow unbounded.
    void flush_per_checkpoint_to_sink(indexer::IndexBatchSink& sink,
                                      int file_id);

    /// Emit file-level records (file_bloom, scalar_stats, counts,
    /// dimensions, name_dictionary, name_file_postings) from the persistent
    /// accumulator. Call once at end-of-file.
    void finalize_file_to_sink(indexer::IndexBatchSink& sink, int file_id);

    std::size_t num_chunks() const { return chunks_base_idx_ + chunks_.size(); }

    /// Total event count across already-flushed chunks plus the currently
    /// buffered ones. Reflects all events ingested via on_event() so far.
    std::uint64_t total_events() const {
        std::uint64_t total = file_acc_.statistics.total_events;
        for (const auto& chunk : chunks_) {
            total += chunk.statistics.total_events;
        }
        return total;
    }

   private:
    void ensure_chunk(std::size_t checkpoint_idx);

    ChunkIndexerConfig config_;
    std::vector<std::string> extra_dim_names_;
    std::vector<ChunkState> chunks_;
    /// Number of checkpoints already flushed and dropped from `chunks_`.
    /// `chunks_[i]` represents checkpoint `chunks_base_idx_ + i`.
    std::size_t chunks_base_idx_ = 0;

    struct FileAccumulator {
        std::array<indexing::BloomFilter, BF_COUNT> fixed_blooms;
        std::vector<indexing::BloomFilter> extra_blooms;
        ChunkStatistics statistics;
        std::size_t num_chunks_emitted = 0;
        bool initialized = false;
    };
    FileAccumulator file_acc_;

    std::uint64_t last_pid_ = UINT64_MAX;
    std::uint64_t last_tid_ = UINT64_MAX;
    char last_pid_buf_[24] = {};
    char last_tid_buf_[24] = {};
    std::uint8_t last_pid_len_ = 0;
    std::uint8_t last_tid_len_ = 0;
};

}  // namespace dftracer::utils::utilities::composites::dft::visitors

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VISITORS_BLOOM_VISITOR_H
