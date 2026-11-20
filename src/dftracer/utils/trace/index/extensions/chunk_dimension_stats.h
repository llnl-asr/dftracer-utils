// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_CHUNK_DIMENSION_STATS_H
#define DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_CHUNK_DIMENSION_STATS_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/trace/index/extensions/summaries.h>

#include <cmath>
#include <cstdint>

namespace dftracer::utils::trace::index::extensions {

/// The text a number has as a counts key, shared by the index and the pruner:
/// an integral value as a plain integer, any other as dstd::to_string. A
/// string path's bloom filter holds the text of a number too, hashed as a
/// string. Distinct values may share a text, which only makes a probe answer
/// "may match".
inline dstd::String canonical_number_text(double v) {
    constexpr double INT64_BOUND = 9223372036854775808.0;
    if (std::isfinite(v) && std::trunc(v) == v && v >= -INT64_BOUND &&
        v < INT64_BOUND)
        return dstd::to_string(static_cast<std::int64_t>(v));
    return dstd::to_string(v);
}

/// What the index keeps about one dimension of one chunk: its value range
/// (@ref ZonemapBuilder) and, for the dimensions that count, how often each
/// value occurs (@ref CountsBuilder).
struct ChunkDimensionStats {
    dstd::String dimension;  ///< Dimension name (e.g., "cat", "name").
    /// Observations, so records that carry the dimension.
    std::uint64_t present = 0;
    ZonemapBuilder zone;
    CountsBuilder counts;
    /// Whether the text of a value is an unsigned integer, so its bounds
    /// compare as numbers (pid and tid).
    bool numeric_text = false;

    static constexpr std::uint32_t NO_SLOT = CountsBuilder::NO_SLOT;

    /// Record a value observation. Updates the bounds and the counts.
    void observe(dstd::StringView value);

    /// As observe. `slot` is where `value` sits in the counts. NO_SLOT finds
    /// it by the string and sets it. Keep a slot for one dimension and one
    /// value.
    void observe(dstd::StringView value, std::uint32_t& slot);
    void observe_range_only(std::uint64_t value);

   private:
    void add_bound(dstd::StringView value);
};

}  // namespace dftracer::utils::trace::index::extensions

#endif
