#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_STATISTICS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_STATISTICS_H

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dftracer::utils::utilities::composites::dft::indexing {

/**
 * @brief Per-chunk statistics for DFTracer events.
 *
 * Tracks event counts by category/name/pid:tid, timestamp ranges,
 * and duration statistics using Welford's online algorithm for variance.
 * Map fields serialize to JSON TEXT for SQLite storage via yyjson.
 */
struct ChunkStatistics {
    std::uint64_t total_events = 0;
    std::unordered_map<std::string, std::uint64_t> category_counts;
    std::unordered_map<std::string, std::uint64_t> name_counts;
    std::unordered_map<std::string, std::uint64_t> pid_tid_counts;

    std::uint64_t min_timestamp_us = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_timestamp_us = 0;
    std::uint64_t duration_count = 0;
    std::int64_t duration_sum_us = 0;
    std::uint64_t duration_min_us = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t duration_max_us = 0;
    double duration_m2 = 0.0;

    void update_from_event(std::string_view name, std::string_view cat,
                           std::uint64_t pid, std::uint64_t tid,
                           std::uint64_t ts, std::uint64_t dur);

    void merge_from(const ChunkStatistics& other);

    double duration_mean() const;
    double duration_variance() const;

    std::string category_counts_json() const;
    std::string name_counts_json() const;
    std::string pid_tid_counts_json() const;

    static std::unordered_map<std::string, std::uint64_t> parse_counts_json(
        const std::string& json);
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_CHUNK_STATISTICS_H
