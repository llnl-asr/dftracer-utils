#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_CONFIG_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_CONFIG_H

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

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
    bool normalize_time = false;  // Normalize time_bucket to 0-based time_range

    std::vector<std::string> extra_group_keys;
    std::vector<std::string> custom_metric_fields;

    bool track_default_args = true;

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

    // Compute hash of config fields that affect aggregation output.
    // Used to detect if cached aggregation data matches current config.
    std::uint32_t compute_hash() const {
        hash::Fnv1aHashBuilder h;

        h.update_value(time_interval_us);
        h.update_value(use_relative_time);
        if (use_relative_time) {
            h.update_value(reference_timestamp);
        }

        for (const auto& k : extra_group_keys) {
            h.update(k);
        }
        for (const auto& m : custom_metric_fields) {
            h.update(m);
        }

        h.update_value(track_default_args);
        h.update_value(compute_statistics);
        h.update_value(compute_percentiles);

        if (compute_percentiles) {
            h.update_value(sketch_accuracy);
            for (double p : percentiles) {
                h.update_value(p);
            }
        }

        for (const auto& be : boundary_events) {
            h.update(be.event_name);
            h.update(be.value_field);
            h.update(be.output_name);
        }

        h.update_value(track_process_parents);

        return h.finish32();
    }
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_CONFIG_H
