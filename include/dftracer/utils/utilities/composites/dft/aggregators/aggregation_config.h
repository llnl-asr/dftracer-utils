#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_CONFIG_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

struct BoundaryEventConfig {
    std::string event_name;
    std::string value_field;
    std::string output_name;
};

struct AggregationConfig {
    std::uint64_t time_interval_us = 1000000;
    bool use_relative_time = false;
    std::uint64_t reference_timestamp = 0;

    std::vector<std::string> extra_group_keys;
    std::vector<std::string> custom_metric_fields;

    bool compute_statistics = true;

    bool compute_percentiles = false;
    double sketch_accuracy = 0.01;
    std::vector<double> percentiles = {0.25, 0.5, 0.75, 0.90};

    std::vector<BoundaryEventConfig> boundary_events;
    bool track_process_parents = true;

    std::string output_format = FORMAT_JSON;

    static constexpr const char* FORMAT_JSON = "json";
    static constexpr const char* FORMAT_ARROW = "arrow";

    static bool is_valid_format(const std::string& fmt) {
        return fmt == FORMAT_JSON
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
               || fmt == FORMAT_ARROW
#endif
            ;
    }

    static std::string supported_formats_str() {
        std::string s = "'json'";
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
        s += " or 'arrow'";
#endif
        return s;
    }
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_CONFIG_H
