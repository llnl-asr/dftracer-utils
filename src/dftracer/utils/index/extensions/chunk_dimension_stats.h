// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_CHUNK_DIMENSION_STATS_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_CHUNK_DIMENSION_STATS_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/dstd/vector.h>

#include <cmath>
#include <cstdint>

namespace dftracer::utils::index::extensions {

/// Per-dimension per-chunk metadata for query optimization.
/// The text a number has in a bloom filter, shared by the index and the
/// pruner's probe: an integral value as a plain integer, any other as
/// dstd::to_string. Distinct values may share a text, which only makes the
/// probe answer "may match".
inline dstd::String canonical_number_text(double v) {
    constexpr double INT64_BOUND = 9223372036854775808.0;
    if (std::isfinite(v) && std::trunc(v) == v && v >= -INT64_BOUND &&
        v < INT64_BOUND)
        return dstd::to_string(static_cast<std::int64_t>(v));
    return dstd::to_string(v);
}

/// Orders two value texts of a dimension by its value_type: numerically for
/// "uint"/"int"/"double", bytewise otherwise.
bool dimension_value_less(dstd::StringView a, dstd::StringView b,
                          dstd::StringView value_type);

struct ChunkDimensionStats {
    dstd::String dimension;  ///< Dimension name (e.g., "cat", "name").
    std::uint64_t distinct_count = 0;  ///< Number of unique values.
    /// Observations, so records that carry the dimension.
    std::uint64_t present = 0;
    dstd::String
        min_value;  ///< Minimum value (numeric-aware for uint/int/double).
    dstd::String max_value;  ///< Maximum value.
    dstd::String value_type =
        "string";            ///< "string", "uint", "int", or "double".

    dstd::Optional<dftracer::utils::dstd::StringViewMap<std::uint64_t>>
        value_counts;

    /// Skips the hash lookup when the same value is observed back-to-back.
    /// Not copied/moved: a copy would point into the original's nodes.
    const dstd::String* last_key = nullptr;
    std::uint64_t* last_counter = nullptr;

    ChunkDimensionStats() = default;
    ChunkDimensionStats(const ChunkDimensionStats& other)
        : dimension(other.dimension),
          distinct_count(other.distinct_count),
          present(other.present),
          min_value(other.min_value),
          max_value(other.max_value),
          value_type(other.value_type),
          value_counts(other.value_counts) {}
    ChunkDimensionStats(ChunkDimensionStats&& other) noexcept
        : dimension(std::move(other.dimension)),
          distinct_count(other.distinct_count),
          present(other.present),
          min_value(std::move(other.min_value)),
          max_value(std::move(other.max_value)),
          value_type(std::move(other.value_type)),
          value_counts(std::move(other.value_counts)) {
        other.last_key = nullptr;
        other.last_counter = nullptr;
    }
    ChunkDimensionStats& operator=(const ChunkDimensionStats& other) {
        if (this != &other) {
            dimension = other.dimension;
            distinct_count = other.distinct_count;
            present = other.present;
            min_value = other.min_value;
            max_value = other.max_value;
            value_type = other.value_type;
            value_counts = other.value_counts;
            last_key = nullptr;
            last_counter = nullptr;
        }
        return *this;
    }
    ChunkDimensionStats& operator=(ChunkDimensionStats&& other) noexcept {
        if (this != &other) {
            dimension = std::move(other.dimension);
            distinct_count = other.distinct_count;
            present = other.present;
            min_value = std::move(other.min_value);
            max_value = std::move(other.max_value);
            value_type = std::move(other.value_type);
            value_counts = std::move(other.value_counts);
            last_key = nullptr;
            last_counter = nullptr;
            other.last_key = nullptr;
            other.last_counter = nullptr;
        }
        return *this;
    }

    static constexpr std::uint32_t NO_SLOT = UINT32_MAX;

    /// Record a value observation. Updates min/max, distinct_count,
    /// value_counts.
    void observe(dstd::StringView value);

    /// As observe. `slot` is where `value` sits in value_counts. NO_SLOT finds
    /// it by the string and sets it. Keep a slot for one dimension and one
    /// value.
    void observe(dstd::StringView value, std::uint32_t& slot);
    void observe_range_only(std::uint64_t value);

    /// Serialize value_counts to binary format:
    /// [u32 LE entry_count] [u16 LE key_size, key bytes, u64 LE count]*
    dstd::Vector<std::uint8_t> serialize_value_counts() const;

    /// Compress serialized value_counts with zlib.
    /// Returns nullopt if compressed size exceeds cap_bytes.
    dstd::Optional<dstd::Vector<std::uint8_t>> compress_value_counts(
        std::size_t cap_bytes = 4096) const;

    static dftracer::utils::dstd::StringViewMap<std::uint64_t>
    deserialize_value_counts(const std::uint8_t* data, std::size_t len);

    /// Decompress zlib-compressed value_counts, then deserialize.
    static dftracer::utils::dstd::StringViewMap<std::uint64_t>
    decompress_value_counts(const std::uint8_t* data, std::size_t len);
};

}  // namespace dftracer::utils::index::extensions

#endif
