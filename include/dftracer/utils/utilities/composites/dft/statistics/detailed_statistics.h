#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_DETAILED_STATISTICS_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_DETAILED_STATISTICS_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/utilities/common/statistics/ddsketch.h>
#include <dftracer/utils/utilities/common/statistics/log2_histogram.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace dftracer::utils::utilities::composites::dft::statistics {

using common::statistics::Log2Histogram;

/**
 * @brief Reusable building block combining histogram + sketch + sum.
 */
struct DistributionStats {
    Log2Histogram histogram;
    common::statistics::DDSketch sketch{0.01};
    double sum = 0.0;
    double sum_sq = 0.0;

    void update(double value);
    void merge(const DistributionStats& other);
    std::uint64_t count() const;
    double mean() const;
    double stddev() const;
};

/**
 * @brief Per-event I/O metrics: duration, size, bandwidth, offset.
 */
struct IOEventMetrics {
    DistributionStats duration;
    DistributionStats size;
    DistributionStats bandwidth;
    DistributionStats offset;

    void merge(const IOEventMetrics& other);
};

/**
 * @brief Accumulates distribution data during on-demand chunk scanning.
 *
 * Supports optional group-by dimensions (name, cat, pid, tid, fhash, hhash,
 * pid_tid). When group_by is empty, only global duration is tracked.
 */
struct DetailedStatistics {
    // Global duration (all events)
    DistributionStats duration;

    // Per-group-key duration statistics
    StringViewMap<DistributionStats> grouped_duration;

    // Per-group-key I/O metrics (only for groups with I/O events)
    StringViewMap<IOEventMetrics> grouped_io;

    // Maps group key -> category string (e.g. "POSIX", "dlio_benchmark")
    // Used by the display layer to split events by category.
    StringViewMap<std::string> group_key_category;

    // Scan progress
    std::uint64_t events_scanned = 0;
    std::uint64_t chunks_scanned = 0;
    std::uint64_t chunks_skipped = 0;

    void merge(const DetailedStatistics& other);
    std::string to_json() const;
};

}  // namespace dftracer::utils::utilities::composites::dft::statistics

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_DETAILED_STATISTICS_H
