// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_CONFIG_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_CONFIG_H

#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::index::schemas::dft::agg {

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

    bool compute_statistics = true;

    bool compute_percentiles = false;
    double sketch_accuracy = 0.01;
    std::vector<double> percentiles = {0.25, 0.5, 0.75, 0.90};

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
        auto text = [&h](dstd::StringView s) {
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
        h.update_value(compute_statistics);
        h.update_value(compute_percentiles);
        if (compute_percentiles) {
            h.update_value(sketch_accuracy);
            h.update_value(percentiles.size());
            for (double p : percentiles) h.update_value(p);
        }
        // An aggregation built without one of these parts, or that held records
        // of an unknown phase, is stale.
        text("partials+bins+columns+file+phase");
        return h.finish();
    }
};

/// The aggregation every index build makes: the struct defaults with the
/// percentile sketches on.
inline AggregationConfig standard_aggregation_config() {
    AggregationConfig config;
    config.compute_percentiles = true;
    return config;
}

/// The start of the window of `timestamp`, rounded down to a multiple of the
/// interval (counted from the reference timestamp with relative time). An
/// interval of 0 gives one window per timestamp.
inline std::uint64_t compute_time_bucket(std::uint64_t timestamp,
                                         const AggregationConfig& config) {
    if (config.use_relative_time) timestamp -= config.reference_timestamp;
    if (config.time_interval_us == 0) return timestamp;
    return (timestamp / config.time_interval_us) * config.time_interval_us;
}

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_CONFIG_H
