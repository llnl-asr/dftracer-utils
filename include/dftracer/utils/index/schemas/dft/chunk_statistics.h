#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_CHUNK_STATISTICS_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_CHUNK_STATISTICS_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/utilities/common/statistics/ddsketch.h>
#include <dftracer/utils/utilities/common/statistics/log2_histogram.h>
#include <dftracer/utils/utilities/common/statistics/timestamp_histogram.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::index::schemas::dft {

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
    /// The first start above 0: traces record events without a clock (CUDA
    /// activity) at ts 0, which would otherwise start every time axis at 0.
    std::uint64_t min_nonzero_timestamp_us =
        std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_timestamp_us = 0;
    std::uint64_t duration_count = 0;
    std::int64_t duration_sum_us = 0;
    std::uint64_t duration_min_us = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t duration_max_us = 0;
    double duration_m2 = 0.0;

    utilities::common::statistics::DDSketch duration_sketch{0.01};
    utilities::common::statistics::Log2Histogram duration_histogram;
    utilities::common::statistics::TimestampHistogram timestamp_histogram;
    StringViewMap<utilities::common::statistics::DDSketch>
        name_duration_sketches;
    StringViewMap<utilities::common::statistics::Log2Histogram>
        name_duration_histograms;
    StringViewMap<double> name_duration_sums;
    StringViewMap<double> name_duration_sum_sqs;
    StringViewMap<std::string> name_category;

    /// Per-category and per-pid duration DDSketches + sums, so the viz stats
    /// fast path can answer group-by-cat and group-by-pid from stored
    /// aggregates. Pid keys are the decimal pid (matching the viz grouping).
    StringViewMap<utilities::common::statistics::DDSketch>
        cat_duration_sketches;
    StringViewMap<double> cat_duration_sums;
    StringViewMap<utilities::common::statistics::DDSketch>
        pid_duration_sketches;
    StringViewMap<double> pid_duration_sums;

    /// has_dur reports whether the event carried a "dur" field. Event counts
    /// and timestamps cover every non-metadata event; duration aggregates
    /// (global and per-name) count only events with a duration, matching the
    /// viz stats aggregation.
    void update_from_event(std::string_view name, std::string_view cat,
                           std::uint64_t pid, std::uint64_t tid,
                           std::uint64_t ts, std::uint64_t dur,
                           bool has_dur = true);

    void merge_from(const ChunkStatistics& other);

    double duration_mean() const;
    double duration_variance() const;

    std::string name_category_json() const;
    std::string name_duration_histograms_json() const;
    std::string name_duration_sums_json() const;
    std::string name_duration_sum_sqs_json() const;
    std::string cat_duration_sums_json() const;
    std::string pid_duration_sums_json() const;

    /// Serialize per-name DDSketches to a single binary blob.
    std::vector<std::uint8_t> serialize_name_duration_sketches() const;

    /// Serialize an arbitrary key -> DDSketch map to one binary blob (same
    /// format as serialize_name_duration_sketches; used for cat/pid sketches).
    static std::vector<std::uint8_t> serialize_sketch_map(
        const StringViewMap<utilities::common::statistics::DDSketch>& sketches);

    static StringViewMap<std::string> parse_string_map_json(
        const std::string& json);
    static StringViewMap<double> parse_double_map_json(const std::string& json);
    static StringViewMap<utilities::common::statistics::Log2Histogram>
    parse_histogram_map_json(const std::string& json);
    static StringViewMap<utilities::common::statistics::DDSketch>
    deserialize_name_duration_sketches(const std::uint8_t* data,
                                       std::size_t len);
};

}  // namespace dftracer::utils::index::schemas::dft

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_CHUNK_STATISTICS_H
