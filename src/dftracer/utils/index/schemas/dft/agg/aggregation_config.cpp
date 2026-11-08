// SPDX-License-Identifier: MIT
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

namespace dftracer::utils::index::schemas::dft::agg {

std::uint64_t AggregationConfig::params_hash() const {
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
    text("partials+bins+columns+file+phase+file_partials+xxh3");
    return h.finish();
}

}  // namespace dftracer::utils::index::schemas::dft::agg
