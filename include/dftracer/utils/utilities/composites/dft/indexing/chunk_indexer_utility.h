#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_INDEXER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_INDEXER_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <algorithm>
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
    bool build_manifest = false;

    // Compute a hash of this config for change detection
    std::size_t compute_hash() const {
        utilities::hash::HasherUtility hasher;
        hasher.update(index_name);
        hasher.update(index_cat);
        hasher.update(index_pid);
        hasher.update(index_tid);
        hasher.update(index_hhash);
        hasher.update(index_fhash);
        hasher.update(index_shash);
        for (const auto& dim : extra_dimensions) {
            hasher.update(dim);
        }
        hasher.update(expected_entries_per_chunk);
        hasher.update(false_positive_rate);
        hasher.update(build_manifest);
        return hasher.get_hash().value;
    }
};

// Hash resolution maps (collected once per file from metadata events)
using HashResolveMap =
    std::shared_ptr<std::unordered_map<std::string, std::string>>;

// Hash resolution entry: dimension -> {hash -> resolved_value}
using HashResolutions =
    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::string>>;

// Tracks which dimensions have been indexed per chunk for incremental updates
struct IndexedDimensions {
    std::vector<std::string> dimensions;

    bool has_dimension(const std::string& dim) const {
        return std::find(dimensions.begin(), dimensions.end(), dim) !=
               dimensions.end();
    }

    void add_dimension(const std::string& dim) {
        if (!has_dimension(dim)) {
            dimensions.push_back(dim);
        }
    }

    // Compute missing dimensions from a target configuration
    std::vector<std::string> missing_dimensions(
        const ChunkIndexerConfig& config) const {
        std::vector<std::string> missing;

        auto check_dim = [this, &missing](const std::string& name,
                                          bool enabled) {
            if (enabled && !has_dimension(name)) {
                missing.emplace_back(name);
            }
        };

        check_dim(std::string("name"), config.index_name);
        check_dim(std::string("cat"), config.index_cat);
        check_dim(std::string("pid"), config.index_pid);
        check_dim(std::string("tid"), config.index_tid);
        check_dim(std::string("hhash"), config.index_hhash);
        check_dim(std::string("fhash"), config.index_fhash);
        check_dim(std::string("shash"), config.index_shash);

        for (const auto& dim : config.extra_dimensions) {
            check_dim(dim, true);
        }

        return missing;
    }
};

// Per-chunk index state for incremental re-scanning
struct ChunkIndexState {
    std::uint64_t checkpoint_idx = 0;
    std::size_t events_processed = 0;
    IndexedDimensions indexed_dims;
    HashResolutions hash_resolutions;
    ChunkStatistics statistics;
    std::size_t config_hash = 0;  // Detect config changes across re-scans
};

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

    // Existing chunk state for incremental re-scanning
    std::shared_ptr<ChunkIndexState> existing_state;

    ChunkIndexerInput& with_existing_state(
        std::shared_ptr<ChunkIndexState> state) {
        existing_state = std::move(state);
        return *this;
    }
};

struct EventLineGroup {
    std::string cat;
    std::string name;
    std::vector<std::uint32_t> line_numbers;
};

struct MetadataLineGroup {
    std::string meta_type;
    std::vector<std::uint32_t> line_numbers;
};

struct ChunkIndexerOutput {
    std::uint64_t checkpoint_idx = 0;
    std::unordered_map<std::string, BloomFilter> bloom_filters;
    ChunkStatistics statistics;
    HashResolutions hash_resolutions;
    std::size_t events_processed = 0;
    bool success = false;
    std::vector<EventLineGroup> event_line_groups;
    std::vector<MetadataLineGroup> metadata_line_groups;
};

class ChunkIndexerUtility
    : public utilities::Utility<ChunkIndexerInput, ChunkIndexerOutput,
                                utilities::tags::Parallelizable> {
   public:
    ChunkIndexerUtility() = default;

    coro::CoroTask<ChunkIndexerOutput> process(
        const ChunkIndexerInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_INDEXER_UTILITY_H
