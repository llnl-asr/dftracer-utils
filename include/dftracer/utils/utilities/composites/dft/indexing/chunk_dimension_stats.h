#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_DIMENSION_STATS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_DIMENSION_STATS_H

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

struct ChunkDimensionStats {
    std::string dimension;
    std::uint64_t distinct_count = 0;
    std::string min_value;
    std::string max_value;
    std::string value_type = "string";

    // NULL when compressed size exceeds cap
    std::optional<std::unordered_map<std::string, std::uint64_t>> value_counts;

    void observe(std::string_view value);

    /// Serialize value_counts to binary format:
    /// [u32 LE num_entries] [u16 LE key_len, key bytes, u64 LE count]*
    std::vector<uint8_t> serialize_value_counts() const;

    /// Compress serialized value_counts with zlib.
    /// Returns nullopt if compressed size exceeds cap_bytes.
    std::optional<std::vector<uint8_t>> compress_value_counts(
        std::size_t cap_bytes = 4096) const;

    static std::unordered_map<std::string, std::uint64_t>
    deserialize_value_counts(const uint8_t* data, std::size_t len);

    /// Decompress zlib-compressed value_counts, then deserialize.
    static std::unordered_map<std::string, std::uint64_t>
    decompress_value_counts(const uint8_t* data, std::size_t len);
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
