#ifndef DFTRACER_UTILS_TRACE_INDEXING_CHUNK_INDEXER_UTILITY_H
#define DFTRACER_UTILS_TRACE_INDEXING_CHUNK_INDEXER_UTILITY_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/trace/indexing/chunk_statistics.h>
#include <dftracer/utils/trace/indexing/scalable_bloom_filter.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::trace::indexing {

struct ChunkIndexerConfig {
    bool index_name = true;
    bool index_cat = true;
    bool index_pid = true;
    bool index_tid = true;
    bool index_hhash = true;
    bool index_fhash = true;
    bool index_shash = true;

    /// Optional user-specified dimensions (arbitrary dot-paths into args)
    /// e.g. "args.level", "args.mode", "args.io.size"
    std::vector<std::string> extra_dimensions;

    /// Also index every other flat args key: numbers get a per-chunk
    /// min/max, strings a per-chunk bloom while the chunk holds at most
    /// auto_max_distinct of their values. On by default, so every build path
    /// (explicit or a query's first touch) makes the same index.
    bool auto_fields = true;
    std::size_t auto_max_distinct = 256;

    std::size_t expected_entries_per_chunk = 1024;
    double false_positive_rate = 0.01;

    /// Events per sub-chunk zone-map bucket (0 disables sub-chunk zone-maps).
    /// A member holds ceil(member_events / sub_chunk_events) buckets.
    std::size_t sub_chunk_events = 4096;

    /// Max compressed size for value_counts BLOB (0 = disable dictionaries)
    std::size_t value_counts_cap = 4096;

    /// Compute a hash of this config for change detection
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
        hasher.update(sub_chunk_events);
        hasher.update(auto_fields);
        hasher.update(auto_max_distinct);
        return hasher.get_hash().value;
    }
};

/// Recorded among a file's index dimensions when auto_fields built it, so a
/// request for auto fields can tell such an index from an older one.
inline constexpr std::string_view AUTO_FIELDS_MARKER = "@auto";

/// A field as the index names its dimension: an args key or a dotted path
/// below args, without the "args." prefix a query column carries.
inline std::string extra_dimension_name(std::string_view field) {
    if (field.rfind("args.", 0) == 0) field.remove_prefix(5);
    return std::string(field);
}

/// Hash resolution maps (collected once per file from metadata events)
using HashResolveMap = std::shared_ptr<StringViewMap<std::string>>;

/// Hash resolution entry: dimension -> {hash -> resolved_value}
using HashResolutions = StringViewMap<StringViewMap<std::string>>;

/// Tracks which dimensions have been indexed per chunk for incremental updates
struct IndexedDimensions {
    std::vector<std::string> dimensions;

    bool has_dimension(const std::string& dim) const {
        return std::find(dimensions.begin(), dimensions.end(), dim) !=
               dimensions.end();
    }

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

/// Per-chunk index state for incremental re-scanning
struct ChunkIndexState {
    std::uint64_t checkpoint_idx = 0;
    std::size_t events_processed = 0;
    IndexedDimensions indexed_dims;
    HashResolutions hash_resolutions;
    ChunkStatistics statistics;
    std::size_t config_hash = 0;  ///< Detect config changes across re-scans
};

struct ChunkIndexerInput {
    std::string file_path;
    std::string index_path;
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

    ChunkIndexerInput& with_index_path(const std::string& path) {
        index_path = path;
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

    /// Existing chunk state for incremental re-scanning
    std::shared_ptr<ChunkIndexState> existing_state;
};

struct ChunkIndexerOutput {
    std::uint64_t checkpoint_idx = 0;
    std::unordered_map<std::string, ScalableBloomFilter> bloom_filters;
    ChunkStatistics statistics;
    HashResolutions hash_resolutions;
    std::size_t events_processed = 0;
    bool success = false;
};

struct ChunkIndexerUtility {
    coro::CoroTask<ChunkIndexerOutput> operator()(
        const ChunkIndexerInput& input);
};

}  // namespace dftracer::utils::trace::indexing

#endif  // DFTRACER_UTILS_TRACE_INDEXING_CHUNK_INDEXER_UTILITY_H
