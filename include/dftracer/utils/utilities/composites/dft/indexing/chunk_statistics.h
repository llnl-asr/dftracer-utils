#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_STATISTICS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_STATISTICS_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/utilities/common/statistics/ddsketch.h>
#include <dftracer/utils/utilities/common/statistics/log2_histogram.h>
#include <dftracer/utils/utilities/common/statistics/timestamp_histogram.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

/**
 * @brief Per-chunk statistics for DFTracer events.
 *
 * Tracks event counts by category/name/pid:tid, timestamp ranges,
 * and duration statistics using Welford's online algorithm for variance.
 * Map fields serialize to JSON text for storage in the
 * shared `.dftindex` database.
 */
struct ChunkStatistics {
    std::uint64_t total_events = 0;
    StringViewMap<std::uint64_t> category_counts;
    StringViewMap<std::uint64_t> name_counts;
    StringViewMap<std::uint64_t> pid_tid_counts;

    std::uint64_t min_timestamp_us = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_timestamp_us = 0;
    std::uint64_t duration_count = 0;
    std::int64_t duration_sum_us = 0;
    std::uint64_t duration_min_us = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t duration_max_us = 0;
    double duration_m2 = 0.0;

    common::statistics::DDSketch duration_sketch{0.01};
    common::statistics::Log2Histogram duration_histogram;
    common::statistics::TimestampHistogram timestamp_histogram;
    StringViewMap<common::statistics::DDSketch> name_duration_sketches;
    StringViewMap<common::statistics::Log2Histogram> name_duration_histograms;
    StringViewMap<double> name_duration_sums;
    StringViewMap<double> name_duration_sum_sqs;
    StringViewMap<std::string> name_category;

    void update_from_event(std::string_view name, std::string_view cat,
                           std::uint64_t pid, std::uint64_t tid,
                           std::uint64_t ts, std::uint64_t dur);

    void merge_from(const ChunkStatistics& other);

    double duration_mean() const;
    double duration_variance() const;

    std::string name_category_json() const;
    std::string name_duration_histograms_json() const;
    std::string name_duration_sums_json() const;
    std::string name_duration_sum_sqs_json() const;

    /// Serialize per-name DDSketches to a single binary blob.
    std::vector<std::uint8_t> serialize_name_duration_sketches() const;

    static StringViewMap<std::string> parse_string_map_json(
        const std::string& json);
    static StringViewMap<double> parse_double_map_json(const std::string& json);
    static StringViewMap<common::statistics::Log2Histogram>
    parse_histogram_map_json(const std::string& json);
    static StringViewMap<common::statistics::DDSketch>
    deserialize_name_duration_sketches(const std::uint8_t* data,
                                       std::size_t len);
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_STATISTICS_H
