#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_INDEXER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_INDEXER_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

struct ChunkIndexerConfig {
    bool index_name = true;
    bool index_cat = true;
    bool index_pid = true;
    bool index_tid = true;
    bool index_hhash = true;
    bool index_fhash = true;
    bool index_shash = true;

    // Optional user-specified dimensions (arbitrary dot-paths into args)
    // e.g. "args.level", "args.mode", "args.io.size"
    std::vector<std::string> extra_dimensions;

    std::size_t expected_entries_per_chunk = 1024;
    double false_positive_rate = 0.01;
};

// Hash resolution maps (collected once per file from metadata events)
using HashResolveMap =
    std::shared_ptr<std::unordered_map<std::string, std::string>>;

struct ChunkIndexerInput {
    std::string file_path;
    std::string idx_path;
    std::size_t checkpoint_size = 0;
    std::uint64_t checkpoint_idx = 0;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    ChunkIndexerConfig config;
    std::size_t batch_size = 4 * 1024 * 1024;
    HashResolveMap hhash_map;
    HashResolveMap fhash_map;
    HashResolveMap shash_map;

    ChunkIndexerInput& with_file_path(const std::string& path) {
        file_path = path;
        return *this;
    }

    ChunkIndexerInput& with_idx_path(const std::string& path) {
        idx_path = path;
        return *this;
    }

    ChunkIndexerInput& with_checkpoint_size(std::size_t size) {
        checkpoint_size = size;
        return *this;
    }

    ChunkIndexerInput& with_checkpoint_idx(std::uint64_t idx) {
        checkpoint_idx = idx;
        return *this;
    }

    ChunkIndexerInput& with_byte_range(std::size_t start, std::size_t end) {
        start_byte = start;
        end_byte = end;
        return *this;
    }

    ChunkIndexerInput& with_config(const ChunkIndexerConfig& cfg) {
        config = cfg;
        return *this;
    }

    ChunkIndexerInput& with_batch_size(std::size_t size) {
        batch_size = size;
        return *this;
    }

    ChunkIndexerInput& with_hash_maps(HashResolveMap hh, HashResolveMap fh,
                                      HashResolveMap sh) {
        hhash_map = std::move(hh);
        fhash_map = std::move(fh);
        shash_map = std::move(sh);
        return *this;
    }
};

// Hash resolution entry: dimension -> {hash -> resolved_value}
using HashResolutions =
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::string>>;

struct ChunkIndexerOutput {
    std::uint64_t checkpoint_idx = 0;
    std::unordered_map<std::string, BloomFilter> bloom_filters;
    ChunkStatistics statistics;
    HashResolutions hash_resolutions;
    std::size_t events_processed = 0;
    bool success = false;
};

class ChunkIndexerUtility
    : public utilities::Utility<ChunkIndexerInput, ChunkIndexerOutput,
                                utilities::tags::Parallelizable> {
   public:
    ChunkIndexerUtility() = default;

    ChunkIndexerOutput process(const ChunkIndexerInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_INDEXER_UTILITY_H
