#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_DIMENSION_STATS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_DIMENSION_STATS_H

#include <dftracer/utils/core/common/transparent_string_hash.h>

#include <cstdint>
#include <optional>
#include <string>
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

    std::optional<dftracer::utils::StringViewMap<std::uint64_t>> value_counts;

    // Skips the hash lookup when the same value is observed back-to-back.
    // Not copied/moved: a copy would point into the original's nodes.
    const std::string* last_key_ = nullptr;
    std::uint64_t* last_counter_ = nullptr;

    ChunkDimensionStats() = default;
    ChunkDimensionStats(const ChunkDimensionStats& other)
        : dimension(other.dimension),
          distinct_count(other.distinct_count),
          min_value(other.min_value),
          max_value(other.max_value),
          value_type(other.value_type),
          value_counts(other.value_counts) {}
    ChunkDimensionStats(ChunkDimensionStats&& other) noexcept
        : dimension(std::move(other.dimension)),
          distinct_count(other.distinct_count),
          min_value(std::move(other.min_value)),
          max_value(std::move(other.max_value)),
          value_type(std::move(other.value_type)),
          value_counts(std::move(other.value_counts)) {
        other.last_key_ = nullptr;
        other.last_counter_ = nullptr;
    }
    ChunkDimensionStats& operator=(const ChunkDimensionStats& other) {
        if (this != &other) {
            dimension = other.dimension;
            distinct_count = other.distinct_count;
            min_value = other.min_value;
            max_value = other.max_value;
            value_type = other.value_type;
            value_counts = other.value_counts;
            last_key_ = nullptr;
            last_counter_ = nullptr;
        }
        return *this;
    }
    ChunkDimensionStats& operator=(ChunkDimensionStats&& other) noexcept {
        if (this != &other) {
            dimension = std::move(other.dimension);
            distinct_count = other.distinct_count;
            min_value = std::move(other.min_value);
            max_value = std::move(other.max_value);
            value_type = std::move(other.value_type);
            value_counts = std::move(other.value_counts);
            last_key_ = nullptr;
            last_counter_ = nullptr;
            other.last_key_ = nullptr;
            other.last_counter_ = nullptr;
        }
        return *this;
    }

    /// Record a value observation. Updates min/max, distinct_count,
    /// value_counts.
    void observe(std::string_view value);
    void observe_range_only(std::uint64_t value);

    /// Serialize value_counts to binary format:
    /// [u32 LE num_entries] [u16 LE key_len, key bytes, u64 LE count]*
    std::vector<std::uint8_t> serialize_value_counts() const;

    /// Compress serialized value_counts with zlib.
    /// Returns nullopt if compressed size exceeds cap_bytes.
    std::optional<std::vector<std::uint8_t>> compress_value_counts(
        std::size_t cap_bytes = 4096) const;

    static dftracer::utils::StringViewMap<std::uint64_t>
    deserialize_value_counts(const std::uint8_t* data, std::size_t len);

    /// Decompress zlib-compressed value_counts, then deserialize.
    static dftracer::utils::StringViewMap<std::uint64_t>
    decompress_value_counts(const std::uint8_t* data, std::size_t len);
};

/// Result type for querying chunk_dimension_stats from the shared index DB.
struct ChunkDimensionStatsResult {
    std::uint64_t checkpoint_idx;
    std::string dimension;
    std::uint64_t distinct_count;
    std::string min_value;
    std::string max_value;
    std::string value_type;
    mutable std::optional<dftracer::utils::StringViewMap<std::uint64_t>>
        value_counts;
    // Raw compressed value_counts. Populated when value_counts is left
    // un-decoded so callers can lazily decode on first access.
    mutable std::vector<std::uint8_t> compressed_value_counts;

    bool has_value_counts_payload() const {
        return value_counts.has_value() || !compressed_value_counts.empty();
    }

    void ensure_value_counts_decoded() const {
        if (value_counts || compressed_value_counts.empty()) return;
        value_counts = ChunkDimensionStats::decompress_value_counts(
            compressed_value_counts.data(), compressed_value_counts.size());
        compressed_value_counts.clear();
        compressed_value_counts.shrink_to_fit();
    }
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif
