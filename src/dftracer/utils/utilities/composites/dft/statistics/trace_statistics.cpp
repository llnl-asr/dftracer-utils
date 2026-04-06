#include <dftracer/utils/utilities/composites/dft/statistics/trace_statistics.h>
#include <yyjson.h>

#include <cmath>

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
void add_counts_object(
    yyjson_mut_doc* doc, yyjson_mut_val* parent, const char* key,
    const std::unordered_map<std::string, std::uint64_t>& m) {
    yyjson_mut_val* obj = yyjson_mut_obj(doc);
    for (const auto& [k, v] : m) {
        yyjson_mut_obj_add_uint(doc, obj, k.c_str(), v);
    }
    yyjson_mut_obj_add_val(doc, parent, key, obj);
}
}  // namespace

std::string TraceStatistics::to_json() const {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_str(doc, root, "file_path", file_path.c_str());
    yyjson_mut_obj_add_str(doc, root, "index_path", index_path.c_str());
    yyjson_mut_obj_add_bool(doc, root, "success", success);

    if (!success) {
        yyjson_mut_obj_add_str(doc, root, "error", error_message.c_str());
    } else {
        yyjson_mut_obj_add_uint(doc, root, "num_chunks", num_chunks);
        yyjson_mut_obj_add_uint(doc, root, "total_events", total_events());
        yyjson_mut_obj_add_uint(doc, root, "num_categories", num_categories());
        yyjson_mut_obj_add_uint(doc, root, "num_unique_names",
                                num_unique_names());
        yyjson_mut_obj_add_uint(doc, root, "num_pid_tids", num_pid_tids());

        // Time range
        yyjson_mut_val* time_range = yyjson_mut_obj(doc);
        if (merged.min_timestamp_us !=
            std::numeric_limits<std::uint64_t>::max()) {
            yyjson_mut_obj_add_uint(doc, time_range, "min_timestamp_us",
                                    merged.min_timestamp_us);
            yyjson_mut_obj_add_uint(doc, time_range, "max_timestamp_us",
                                    merged.max_timestamp_us);
        }
        yyjson_mut_obj_add_real(doc, time_range, "time_span_seconds",
                                time_span_seconds());
        yyjson_mut_obj_add_val(doc, root, "time_range", time_range);

        // Duration stats
        yyjson_mut_val* duration = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, duration, "count", merged.duration_count);
        if (merged.duration_count > 0) {
            yyjson_mut_obj_add_int(doc, duration, "sum_us",
                                   merged.duration_sum_us);
            yyjson_mut_obj_add_real(doc, duration, "mean_us",
                                    duration_mean_us());
            yyjson_mut_obj_add_real(doc, duration, "stddev_us",
                                    duration_stddev_us());
            if (merged.duration_min_us !=
                std::numeric_limits<std::uint64_t>::max()) {
                yyjson_mut_obj_add_uint(doc, duration, "min_us",
                                        merged.duration_min_us);
            }
            yyjson_mut_obj_add_uint(doc, duration, "max_us",
                                    merged.duration_max_us);
        }
        yyjson_mut_obj_add_val(doc, root, "duration", duration);

        // Count maps
        add_counts_object(doc, root, "category_counts", merged.category_counts);
        add_counts_object(doc, root, "name_counts", merged.name_counts);
        add_counts_object(doc, root, "pid_tid_counts", merged.pid_tid_counts);
    }

    char* json_str = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, nullptr);
    std::string result(json_str ? json_str : "{}");
    if (json_str) free(json_str);
    yyjson_mut_doc_free(doc);
    return result;
}

}  // namespace dftracer::utils::utilities::composites::dft::statistics
