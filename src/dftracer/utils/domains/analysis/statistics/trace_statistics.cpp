// SPDX-License-Identifier: MIT
#include <dftracer/utils/domains/analysis/statistics/trace_statistics.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/internal/utils.h>

#include <cmath>

namespace dftracer::utils::domains::analysis::statistics {

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

std::size_t TraceStatistics::category_count() const {
    return merged.category_counts.size();
}

std::size_t TraceStatistics::unique_name_count() const {
    return merged.name_counts.size();
}

std::size_t TraceStatistics::pid_tid_count() const {
    return merged.pid_tid_counts.size();
}

namespace {
template <typename Map>
void add_counts_object(dstd::String& ss, const char* key, const Map& m,
                       bool& first_field) {
    if (!first_field) ss += ',';
    first_field = false;
    ss += '"';
    trace::internal::append_value(ss, key);
    ss += "\":{";
    bool first = true;
    for (const auto& [k, v] : m) {
        if (!first) ss += ',';
        first = false;
        ss += '"';
        trace::internal::append_value(ss, k);
        ss += "\":";
        trace::internal::append_value(ss, v);
    }
    ss += '}';
}
}  // namespace

dstd::String TraceStatistics::to_json() const {
    dstd::String ss;
    ss += '{';

    ss += "\"file_path\":\"";
    trace::internal::append_value(ss, file_path);
    ss += '"';
    ss += ",\"index_path\":\"";
    trace::internal::append_value(ss, index_path);
    ss += '"';
    ss += ",\"success\":";
    trace::internal::append_value(ss, (success ? "true" : "false"));
    ss += ",\"total_events\":";
    trace::internal::append_value(ss, total_events());

    if (!success) {
        ss += ",\"error\":\"";
        trace::internal::append_value(ss, error_message);
        ss += '"';
    } else {
        ss += ",\"num_chunks\":";
        trace::internal::append_value(ss, chunk_count);
        ss += ",\"num_categories\":";
        trace::internal::append_value(ss, category_count());
        ss += ",\"num_unique_names\":";
        trace::internal::append_value(ss, unique_name_count());
        ss += ",\"num_pid_tids\":";
        trace::internal::append_value(ss, pid_tid_count());

        ss += ",\"time_range\":{";
        if (merged.min_timestamp_us !=
            std::numeric_limits<std::uint64_t>::max()) {
            ss += "\"min_timestamp_us\":";
            trace::internal::append_value(ss, merged.min_timestamp_us);
            ss += ",\"max_timestamp_us\":";
            trace::internal::append_value(ss, merged.max_timestamp_us);
            ss += ',';
        }
        ss += "\"time_span_seconds\":";
        trace::internal::append_value(ss, time_span_seconds());
        ss += '}';

        ss += ",\"duration\":{";
        ss += "\"count\":";
        trace::internal::append_value(ss, merged.duration_count);
        if (merged.duration_count > 0) {
            ss += ",\"sum_us\":";
            trace::internal::append_value(ss, merged.duration_sum_us);
            ss += ",\"mean_us\":";
            trace::internal::append_value(ss, duration_mean_us());
            ss += ",\"stddev_us\":";
            trace::internal::append_value(ss, duration_stddev_us());
            if (merged.duration_min_us !=
                std::numeric_limits<std::uint64_t>::max()) {
                ss += ",\"min_us\":";
                trace::internal::append_value(ss, merged.duration_min_us);
            }
            ss += ",\"max_us\":";
            trace::internal::append_value(ss, merged.duration_max_us);
        }
        ss += '}';

        bool first_field = false;
        add_counts_object(ss, "category_counts", merged.category_counts,
                          first_field);
        add_counts_object(ss, "name_counts", merged.name_counts, first_field);
        add_counts_object(ss, "pid_tid_counts", merged.pid_tid_counts,
                          first_field);
    }

    ss += '}';
    return ss;
}

}  // namespace dftracer::utils::domains::analysis::statistics
