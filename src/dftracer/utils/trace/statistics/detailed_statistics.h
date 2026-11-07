// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_STATISTICS_DETAILED_STATISTICS_H
#define DFTRACER_UTILS_TRACE_STATISTICS_DETAILED_STATISTICS_H

#include <dftracer/utils/dataframe/log2_histogram.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>

#include <cstdint>

namespace dftracer::utils::trace::statistics {

using dataframe::Log2Histogram;

/** @brief A value distribution: log2 histogram, DDSketch, and running sums. */
struct DistributionStats {
    Log2Histogram histogram;
    dataframe::DDSketch sketch{0.01};
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

    dstd::StringViewMap<DistributionStats> grouped_duration;

    /// Only groups that have I/O events.
    dstd::StringViewMap<IOEventMetrics> grouped_io;

    /// Group key -> category (e.g. "POSIX", "dlio_benchmark"); the display
    /// layer splits events by category on this.
    dstd::StringViewMap<dstd::String> group_key_category;

    std::uint64_t events_scanned = 0;
    std::uint64_t chunks_scanned = 0;
    std::uint64_t chunks_skipped = 0;

    void merge(const DetailedStatistics& other);
    dstd::String to_json() const;
};

}  // namespace dftracer::utils::trace::statistics

#endif  // DFTRACER_UTILS_TRACE_STATISTICS_DETAILED_STATISTICS_H
