#ifndef DFTRACER_UTILS_TRACE_STATISTICS_DETAILED_STATISTICS_H
#define DFTRACER_UTILS_TRACE_STATISTICS_DETAILED_STATISTICS_H

#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/utilities/common/statistics/ddsketch.h>
#include <dftracer/utils/utilities/common/statistics/log2_histogram.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace dftracer::utils::trace::statistics {

using utilities::common::statistics::Log2Histogram;

/** @brief A value distribution: log2 histogram, DDSketch, and running sums. */
struct DistributionStats {
    Log2Histogram histogram;
    utilities::common::statistics::DDSketch sketch{0.01};
    double sum = 0.0;
    double sum_sq = 0.0;

    void update(double value);
    void merge(const DistributionStats& other);
    std::uint64_t count() const;
    double mean() const;
    double stddev() const;
};

/** @brief Per-event I/O metric distributions. */
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
 * Group-by dimensions (name, cat, pid, tid, fhash, hhash, pid_tid) are
 * optional; with none, only the global duration is tracked.
 */
struct DetailedStatistics {
    /// Global duration over all events.
    DistributionStats duration;

    StringViewMap<DistributionStats> grouped_duration;

    /// Only groups that have I/O events.
    StringViewMap<IOEventMetrics> grouped_io;

    /// Group key -> category (e.g. "POSIX", "dlio_benchmark"); the display
    /// layer splits events by category on this.
    StringViewMap<std::string> group_key_category;

    std::uint64_t events_scanned = 0;
    std::uint64_t chunks_scanned = 0;
    std::uint64_t chunks_skipped = 0;

    void merge(const DetailedStatistics& other);
    std::string to_json() const;
};

}  // namespace dftracer::utils::trace::statistics

#endif  // DFTRACER_UTILS_TRACE_STATISTICS_DETAILED_STATISTICS_H
