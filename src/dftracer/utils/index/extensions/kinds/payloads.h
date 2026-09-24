#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_KINDS_PAYLOADS_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_KINDS_PAYLOADS_H

#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/utilities/common/statistics/timestamp_histogram.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Per-granule payloads of the built-in pruning kinds. A value that fails to
// decode is treated like a missing one: no evidence.
namespace dftracer::utils::index::extensions::kinds {

/// One path's values in one granule.
struct Zone {
    /// "string", "uint", "int", "double" or "mixed"; bounds compare in it.
    std::string value_type;
    std::string min;
    std::string max;
    /// Records observed in the granule (dftracer: data events).
    std::uint64_t observed = 0;
    /// Observed records carrying the path, when known.
    std::optional<std::uint64_t> present;
    /// Where the path's values fall, for timestamp paths.
    std::optional<utilities::common::statistics::TimestampHistogram> histogram;
};

std::string encode_zone(const Zone& zone);
std::optional<Zone> decode_zone(std::string_view bytes);

/// A granule's value counts: the compressed map, or nullopt when it went over
/// the cap ("cannot prune").
struct CountsGranule {
    std::uint64_t observed = 0;
    std::optional<std::vector<std::uint8_t>> compressed;
};

std::string encode_counts(const CountsGranule& counts);
std::optional<CountsGranule> decode_counts(std::string_view bytes);

std::string encode_bloom(std::span<const unsigned char> bloom,
                         std::uint64_t num_entries);
std::optional<ScalableBloomFilter> decode_bloom(std::string_view bytes);

/// Postings key a value by this hash.
std::uint64_t value_hash(std::string_view value);

}  // namespace dftracer::utils::index::extensions::kinds

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_KINDS_PAYLOADS_H
