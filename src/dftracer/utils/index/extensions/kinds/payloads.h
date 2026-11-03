// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_KINDS_PAYLOADS_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_KINDS_PAYLOADS_H

#include <dftracer/utils/dataframe/timestamp_histogram.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>

#include <cstdint>

// Per-granule payloads of the built-in pruning kinds. A value that fails to
// decode is treated like a missing one: no evidence.
namespace dftracer::utils::index::extensions::kinds {

/// One path's values in one granule.
struct Zone {
    /// "string", "uint", "int", "double" or "mixed"; bounds compare in it.
    dstd::String value_type;
    dstd::String min;
    dstd::String max;
    /// Records observed in the granule (dftracer: data events).
    std::uint64_t observed = 0;
    /// Observed records carrying the path, when known.
    dstd::Optional<std::uint64_t> present;
    /// Where the path's values fall, for timestamp paths.
    dstd::Optional<dataframe::TimestampHistogram> histogram;
};

dstd::String encode_zone(const Zone& zone);
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
                          std::uint64_t num_entries);
dstd::Optional<ScalableBloomFilter> decode_bloom(dstd::StringView bytes);

/// Postings key a value by this hash.
std::uint64_t value_hash(dstd::StringView value);

}  // namespace dftracer::utils::index::extensions::kinds

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_KINDS_PAYLOADS_H
