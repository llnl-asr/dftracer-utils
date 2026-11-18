// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_KINDS_PAYLOADS_H
#define DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_KINDS_PAYLOADS_H

#include <dftracer/utils/dataframe/timestamp_histogram.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/trace/index/extensions/zone_bounds.h>

#include <cstdint>

// Per-granule payloads of the built-in pruning kinds. A value that fails to
// decode is treated like a missing one: no evidence.
namespace dftracer::utils::trace::index::extensions::kinds {

/// The version byte a stored zone starts with. A zone with another one is not
/// read, which prunes nothing.
inline constexpr std::uint8_t ZONE_PAYLOAD_VERSION = 1;

/// One path's values in one granule: the bounds with their null and NaN counts
/// (@ref ZoneBounds), and how many records the granule observed.
struct Zone : ZoneBounds {
    /// Where the path's values fall, for timestamp paths.
    dstd::Optional<dataframe::TimestampHistogram> histogram;
    /// Records observed in the granule (dftracer: data events).
    std::uint64_t observed = 0;
    /// Observed records carrying the path, when known.
    dstd::Optional<std::uint64_t> present;
};

dstd::String encode_zone(const Zone& zone);
/// Nothing for bytes of another version, a bound pair out of order or a bound
/// that is NaN.
dstd::Optional<Zone> decode_zone(dstd::StringView bytes);

/// A granule's value counts: the compressed map, or nullopt when it went over
/// the cap ("cannot prune").
struct CountsGranule {
    std::uint64_t observed = 0;
    dstd::Optional<dstd::Vector<std::uint8_t>> compressed;
};

dstd::String encode_counts(const CountsGranule& counts);
dstd::Optional<CountsGranule> decode_counts(dstd::StringView bytes);

dstd::String encode_bloom(dstd::Span<const unsigned char> bloom,
                          std::uint64_t entry_count);
dstd::Optional<ScalableBloomFilter> decode_bloom(dstd::StringView bytes);

}  // namespace dftracer::utils::trace::index::extensions::kinds

#endif  // DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_KINDS_PAYLOADS_H
