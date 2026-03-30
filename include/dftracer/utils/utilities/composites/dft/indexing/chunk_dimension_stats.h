#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_DIMENSION_STATS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_DIMENSION_STATS_H

#include <dftracer/utils/core/common/transparent_string_hash.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

/// Per-dimension per-chunk metadata for query optimization.
struct ChunkDimensionStats {
    std::string dimension;  ///< Dimension name (e.g., "cat", "name").
    std::uint64_t distinct_count = 0;  ///< Number of unique values.
    std::string
        min_value;  ///< Minimum value (numeric-aware for uint/int/double).
    std::string max_value;  ///< Maximum value.
    std::string value_type =
        "string";           ///< "string", "uint", "int", or "double".

    /// Value -> count map. Nullopt when compressed size exceeds cap.
    /// Uses transparent hash to allow string_view lookups without allocation.
    std::optional<std::unordered_map<std::string, std::uint64_t,
                                     utils::TransparentStringHash,
                                     utils::TransparentStringEqual>>
        value_counts;

    /// Record a value observation. Updates min/max, distinct_count,
    /// value_counts.
    void observe(std::string_view value);

    /// Serialize value_counts to binary format:
    /// [u32 LE num_entries] [u16 LE key_len, key bytes, u64 LE count]*
    std::vector<std::uint8_t> serialize_value_counts() const;

    /// Compress serialized value_counts with zlib.
    /// Returns nullopt if compressed size exceeds cap_bytes.
    std::optional<std::vector<std::uint8_t>> compress_value_counts(
        std::size_t cap_bytes = 4096) const;

    static std::unordered_map<std::string, std::uint64_t>
    deserialize_value_counts(const std::uint8_t* data, std::size_t len);

    /// Decompress zlib-compressed value_counts, then deserialize.
    static std::unordered_map<std::string, std::uint64_t>
    decompress_value_counts(const std::uint8_t* data, std::size_t len);
};

/// Result type for querying chunk_dimension_stats from SQLite.
struct ChunkDimensionStatsResult {
    std::uint64_t checkpoint_idx;
    std::string dimension;
    std::uint64_t distinct_count;
    std::string min_value;
    std::string max_value;
    std::string value_type;
    // NULL in DB → nullopt here
    std::optional<std::unordered_map<std::string, std::uint64_t>> value_counts;
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif
