#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_CONFIG_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_CONFIG_H

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::schemas::dft::agg {

struct BoundaryEventConfig {
    std::string event_name;
    std::string value_field;
    std::string output_name;
};

struct AggregationConfig {
    std::uint64_t time_interval_us = 1000000;
    bool use_relative_time = false;
    std::uint64_t reference_timestamp = 0;
    bool normalize_time =
        false;  ///< Normalize time_bucket to 0-based time_range

    /// Off makes the key coarse enough to aggregate on traces with millions of
    /// files; distinct files are then counted with a sketch.
    bool group_by_file = true;

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

    /// Hash of every field that shapes the stored aggregation: the params hash
    /// of the dftracer.agg extension. Data built with another hash is stale.
    std::uint64_t params_hash() const {
        utilities::hash::Fnv1aHashBuilder h;
        auto text = [&h](std::string_view s) {
            h.update_value(s.size());
            h.update(s);
        };
        h.update_value(time_interval_us);
        h.update_value(use_relative_time);
        if (use_relative_time) h.update_value(reference_timestamp);
        h.update_value(normalize_time);
        h.update_value(group_by_file);
        h.update_value(extra_group_keys.size());
        for (const auto& k : extra_group_keys) text(k);
        h.update_value(custom_metric_fields.size());
        for (const auto& m : custom_metric_fields) text(m);
        h.update_value(track_default_args);
        h.update_value(compute_statistics);
        h.update_value(compute_percentiles);
        if (compute_percentiles) {
            h.update_value(sketch_accuracy);
            h.update_value(percentiles.size());
            for (double p : percentiles) h.update_value(p);
        }
        h.update_value(boundary_events.size());
        for (const auto& be : boundary_events) {
            text(be.event_name);
            text(be.value_field);
            text(be.output_name);
        }
        h.update_value(track_process_parents);
        return h.finish();
    }
};

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_CONFIG_H
