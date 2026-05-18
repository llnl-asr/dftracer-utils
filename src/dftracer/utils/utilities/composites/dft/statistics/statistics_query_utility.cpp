#include <dftracer/utils/utilities/composites/dft/statistics/statistics_query_utility.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace dftracer::utils::utilities::composites::dft::statistics {

namespace {
template <typename Map>
std::vector<std::pair<std::string, std::uint64_t>> sorted_desc(const Map& m) {
    std::vector<std::pair<std::string, std::uint64_t>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return v;
}

template <typename Map>
std::vector<std::pair<std::string, std::uint64_t>> top_n(const Map& m,
                                                         std::uint64_t n) {
    auto v = sorted_desc(m);
    if (n == 0) {
        return v;
    }
    if (v.size() > n) {
        v.resize(static_cast<std::size_t>(n));
    }
    return v;
}

const char* query_type_to_string(StatisticsQueryType t) {
    switch (t) {
        case StatisticsQueryType::SUMMARY:
            return "summary";
        case StatisticsQueryType::CATEGORIES:
            return "categories";
        case StatisticsQueryType::NAMES:
            return "names";
        case StatisticsQueryType::PID_TIDS:
            return "pid_tids";
        case StatisticsQueryType::TIME_RANGE:
            return "time_range";
        case StatisticsQueryType::DURATION_STATS:
            return "duration_stats";
        case StatisticsQueryType::TOP_N_NAMES:
            return "top_n_names";
        case StatisticsQueryType::TOP_N_CATEGORIES:
            return "top_n_categories";
        case StatisticsQueryType::DETAILED:
            return "detailed";
    }
    return "unknown";
}
}  // namespace

coro::CoroTask<StatisticsQueryOutput> StatisticsQueryUtility::process(
    const StatisticsQueryInput& input) {
    StatisticsQueryOutput output;
    const auto& stats = input.stats;
    const auto& merged = stats.merged;

    output.total_events = merged.total_events;
    output.query_type_name = query_type_to_string(input.query_type);

    switch (input.query_type) {
        case StatisticsQueryType::SUMMARY:
            // Populate all fields
            output.results = sorted_desc(merged.category_counts);
            if (merged.min_timestamp_us !=
                std::numeric_limits<std::uint64_t>::max()) {
                output.min_timestamp_us = merged.min_timestamp_us;
                output.max_timestamp_us = merged.max_timestamp_us;
            }
            output.time_span_seconds = stats.time_span_seconds();
            output.duration_count = merged.duration_count;
            output.duration_mean_us = stats.duration_mean_us();
            output.duration_stddev_us = stats.duration_stddev_us();
            if (merged.duration_min_us !=
                std::numeric_limits<std::uint64_t>::max()) {
                output.duration_min_us = merged.duration_min_us;
            }
            output.duration_max_us = merged.duration_max_us;
            break;

        case StatisticsQueryType::CATEGORIES:
            output.results = sorted_desc(merged.category_counts);
            break;

        case StatisticsQueryType::NAMES:
            output.results = sorted_desc(merged.name_counts);
            break;

        case StatisticsQueryType::PID_TIDS:
            output.results = sorted_desc(merged.pid_tid_counts);
            break;

        case StatisticsQueryType::TIME_RANGE:
            if (merged.min_timestamp_us !=
                std::numeric_limits<std::uint64_t>::max()) {
                output.min_timestamp_us = merged.min_timestamp_us;
                output.max_timestamp_us = merged.max_timestamp_us;
            }
            output.time_span_seconds = stats.time_span_seconds();
            break;

        case StatisticsQueryType::DURATION_STATS:
            output.duration_count = merged.duration_count;
            output.duration_mean_us = stats.duration_mean_us();
            output.duration_stddev_us = stats.duration_stddev_us();
            if (merged.duration_min_us !=
                std::numeric_limits<std::uint64_t>::max()) {
                output.duration_min_us = merged.duration_min_us;
            }
            output.duration_max_us = merged.duration_max_us;
            break;

        case StatisticsQueryType::TOP_N_NAMES:
            output.results = top_n(merged.name_counts, input.top_n);
            break;

        case StatisticsQueryType::TOP_N_CATEGORIES:
            output.results = top_n(merged.category_counts, input.top_n);
            break;

        case StatisticsQueryType::DETAILED:
            // Detailed queries bypass StatisticsQueryUtility entirely --
            // they use ChunkDetailScannerUtility for on-demand chunk scanning.
            break;
    }

    co_return output;
}

std::string StatisticsQueryOutput::to_json() const {
    std::ostringstream ss;
    ss << std::setprecision(17);
    ss << '{';

    ss << "\"query_type\":\"" << query_type_name << '"';
    ss << ",\"total_events\":" << total_events;

    if (!results.empty()) {
        ss << ",\"results\":[";
        bool first = true;
        for (const auto& [name, count] : results) {
            if (!first) ss << ',';
            first = false;
            ss << "{\"name\":\"" << name << "\",\"count\":" << count << '}';
        }
        ss << ']';
    }

    if (min_timestamp_us > 0 || max_timestamp_us > 0) {
        ss << ",\"time_range\":{";
        ss << "\"min_timestamp_us\":" << min_timestamp_us;
        ss << ",\"max_timestamp_us\":" << max_timestamp_us;
        ss << ",\"time_span_seconds\":" << time_span_seconds;
        ss << '}';
    }

    if (duration_count > 0) {
        ss << ",\"duration\":{";
        ss << "\"count\":" << duration_count;
        ss << ",\"mean_us\":" << duration_mean_us;
        ss << ",\"stddev_us\":" << duration_stddev_us;
        ss << ",\"min_us\":" << duration_min_us;
        ss << ",\"max_us\":" << duration_max_us;
        ss << '}';
    }

    ss << '}';
    return ss.str();
}

}  // namespace dftracer::utils::utilities::composites::dft::statistics
