#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_VISITORS_BLOOM_VISITOR_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_VISITORS_BLOOM_VISITOR_H

#include <dftracer/utils/utilities/common/json/json.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <dftracer/utils/utilities/indexer/index_visitor.h>
#include <yyjson.h>

#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::indexer {

class BloomVisitor : public IndexVisitor {
   public:
    using BloomFilterMap = std::unordered_map<
        std::string,
        dftracer::utils::utilities::composites::dft::indexing::BloomFilter>;
    using HashResolutions =
        dftracer::utils::utilities::composites::dft::indexing::HashResolutions;
    using ChunkStatistics =
        dftracer::utils::utilities::composites::dft::indexing::ChunkStatistics;
    using ChunkDimensionStats = dftracer::utils::utilities::composites::dft::
        indexing::ChunkDimensionStats;
    using ChunkIndexerConfig = dftracer::utils::utilities::composites::dft::
        indexing::ChunkIndexerConfig;

    struct ChunkState {
        BloomFilterMap bloom_filters;
        ChunkStatistics statistics;
        HashResolutions hash_resolutions;
        std::unordered_map<std::string, ChunkDimensionStats> dimension_stats;
        std::size_t events_processed = 0;
    };

    BloomVisitor(ChunkIndexerConfig config,
                 std::vector<std::string> dimensions);

    void begin(std::size_t num_checkpoints) override;
    void on_checkpoint(std::size_t checkpoint_idx) override;
    void on_line(std::string_view line, std::size_t checkpoint_idx) override;
    void finalize(IndexDatabase& db, int file_id) override;

    std::size_t num_chunks() const { return chunks_.size(); }

   private:
    void ensure_chunk(std::size_t checkpoint_idx);

    ChunkIndexerConfig config_;
    std::vector<std::string> dimensions_;
    std::vector<ChunkState> chunks_;

    std::array<char, common::json::YYJSON_LINE_POOL_SIZE> yy_buf_{};
    yyjson_alc yy_alc_{};
    bool yy_alc_initialized_ = false;
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_VISITORS_BLOOM_VISITOR_H
