#ifndef DFTRACER_UTILS_TRACE_STATISTICS_QUERY_UTILITY_H
#define DFTRACER_UTILS_TRACE_STATISTICS_QUERY_UTILITY_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/trace/statistics/trace_statistics.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::statistics {

enum class StatisticsQueryType {
    Summary,
    Categories,
    Names,
    PidTids,
    TimeRange,
    DurationStats,
    TopNNames,
    TopNCategories,
    Detailed
};

struct StatisticsQueryInput {
    TraceStatistics stats;
    StatisticsQueryType query_type = StatisticsQueryType::Summary;
    std::uint64_t top_n = 10;
};

struct StatisticsQueryOutput {
    std::vector<std::pair<std::string, std::uint64_t>> results;
    std::uint64_t total_events = 0;

    std::uint64_t min_timestamp_us = 0;
    std::uint64_t max_timestamp_us = 0;
    double time_span_seconds = 0.0;

    std::uint64_t duration_count = 0;
    double duration_mean_us = 0.0;
    double duration_stddev_us = 0.0;
    std::uint64_t duration_min_us = 0;
    std::uint64_t duration_max_us = 0;

    std::string query_type_name;

    std::string to_json() const;
};

/// Derives report rows from a TraceStatistics snapshot. A plain async op
/// (StatisticsQueryInput -> CoroTask<StatisticsQueryOutput>); composes and
/// runs anywhere a callable does.
struct StatisticsQueryUtility {
    coro::CoroTask<StatisticsQueryOutput> operator()(
        const StatisticsQueryInput& input) const;
};

}  // namespace dftracer::utils::trace::statistics

#endif  // DFTRACER_UTILS_TRACE_STATISTICS_QUERY_UTILITY_H
