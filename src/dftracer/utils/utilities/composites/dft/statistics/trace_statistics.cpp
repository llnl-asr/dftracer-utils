#include <dftracer/utils/utilities/composites/dft/statistics/trace_statistics.h>

#include <cmath>
#include <iomanip>
#include <sstream>

namespace dftracer::utils::utilities::composites::dft::statistics {

std::uint64_t TraceStatistics::total_events() const {
    return merged.total_events;
}

double TraceStatistics::time_span_seconds() const {
    if (merged.total_events == 0) return 0.0;
    if (merged.min_timestamp_us == std::numeric_limits<std::uint64_t>::max())
        return 0.0;
    if (merged.max_timestamp_us <= merged.min_timestamp_us) return 0.0;
    return static_cast<double>(merged.max_timestamp_us -
                               merged.min_timestamp_us) /
           1e6;
}

double TraceStatistics::duration_mean_us() const {
    return merged.duration_mean();
}

double TraceStatistics::duration_stddev_us() const {
    return std::sqrt(merged.duration_variance());
}

std::size_t TraceStatistics::num_categories() const {
    return merged.category_counts.size();
}

std::size_t TraceStatistics::num_unique_names() const {
    return merged.name_counts.size();
}

std::size_t TraceStatistics::num_pid_tids() const {
    return merged.pid_tid_counts.size();
}

namespace {
template <typename Map>
void add_counts_object(std::ostringstream& ss, const char* key, const Map& m,
                       bool& first_field) {
    if (!first_field) ss << ',';
    first_field = false;
    ss << '"' << key << "\":{";
    bool first = true;
    for (const auto& [k, v] : m) {
        if (!first) ss << ',';
        first = false;
        ss << '"' << k << "\":" << v;
    }
    ss << '}';
}
}  // namespace

std::string TraceStatistics::to_json() const {
    std::ostringstream ss;
    ss << std::setprecision(17);
    ss << '{';

    ss << "\"file_path\":\"" << file_path << '"';
    ss << ",\"index_path\":\"" << index_path << '"';
    ss << ",\"success\":" << (success ? "true" : "false");
    ss << ",\"total_events\":" << total_events();

    if (!success) {
        ss << ",\"error\":\"" << error_message << '"';
    } else {
        ss << ",\"num_chunks\":" << num_chunks;
        ss << ",\"num_categories\":" << num_categories();
        ss << ",\"num_unique_names\":" << num_unique_names();
        ss << ",\"num_pid_tids\":" << num_pid_tids();

        // Time range
        ss << ",\"time_range\":{";
        if (merged.min_timestamp_us !=
            std::numeric_limits<std::uint64_t>::max()) {
            ss << "\"min_timestamp_us\":" << merged.min_timestamp_us;
            ss << ",\"max_timestamp_us\":" << merged.max_timestamp_us;
            ss << ',';
        }
        ss << "\"time_span_seconds\":" << time_span_seconds();
        ss << '}';

        // Duration stats
        ss << ",\"duration\":{";
        ss << "\"count\":" << merged.duration_count;
        if (merged.duration_count > 0) {
            ss << ",\"sum_us\":" << merged.duration_sum_us;
            ss << ",\"mean_us\":" << duration_mean_us();
            ss << ",\"stddev_us\":" << duration_stddev_us();
            if (merged.duration_min_us !=
                std::numeric_limits<std::uint64_t>::max()) {
                ss << ",\"min_us\":" << merged.duration_min_us;
            }
            ss << ",\"max_us\":" << merged.duration_max_us;
        }
        ss << '}';

        // Count maps
        bool first_field = false;
        add_counts_object(ss, "category_counts", merged.category_counts,
                          first_field);
        add_counts_object(ss, "name_counts", merged.name_counts, first_field);
        add_counts_object(ss, "pid_tid_counts", merged.pid_tid_counts,
                          first_field);
    }

    ss << '}';
    return ss.str();
}

}  // namespace dftracer::utils::utilities::composites::dft::statistics
