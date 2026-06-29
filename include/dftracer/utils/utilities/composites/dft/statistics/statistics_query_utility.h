#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_QUERY_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_QUERY_UTILITY_H

#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/statistics/trace_statistics.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::statistics {

enum class StatisticsQueryType {
    SUMMARY,
    CATEGORIES,
    NAMES,
    PID_TIDS,
    TIME_RANGE,
    DURATION_STATS,
    TOP_N_NAMES,
    TOP_N_CATEGORIES,
    DETAILED
};

struct StatisticsQueryInput {
    TraceStatistics stats;
    StatisticsQueryType query_type = StatisticsQueryType::SUMMARY;
    std::uint64_t top_n = 10;
};

struct StatisticsQueryOutput {
    std::vector<std::pair<std::string, std::uint64_t>> results;
    std::uint64_t total_events = 0;

    // Time range fields
    std::uint64_t min_timestamp_us = 0;
    std::uint64_t max_timestamp_us = 0;
    double time_span_seconds = 0.0;

    // Duration fields
    std::uint64_t duration_count = 0;
    double duration_mean_us = 0.0;
    double duration_stddev_us = 0.0;
    std::uint64_t duration_min_us = 0;
    std::uint64_t duration_max_us = 0;

    std::string query_type_name;

    std::string to_json() const;
};

class StatisticsQueryUtility
    : public utilities::Utility<StatisticsQueryInput, StatisticsQueryOutput> {
   public:
    StatisticsQueryUtility() = default;

    coro::CoroTask<StatisticsQueryOutput> process(
        const StatisticsQueryInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::statistics

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_STATISTICS_QUERY_UTILITY_H
