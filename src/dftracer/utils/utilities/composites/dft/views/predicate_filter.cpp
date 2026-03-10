#include <dftracer/utils/utilities/composites/dft/views/predicate_filter.h>

#include <string>

namespace dftracer::utils::utilities::composites::dft::views {

using common::json::JsonValue;

PredicateFilter build_predicate_filter(const ViewPredicate& predicate) {
    PredicateFilter filter;
    for (const auto& [dim, values] : predicate.bloom_dims) {
        std::string resolved_dim = resolve_bloom_dimension(dim);
        std::unordered_set<std::string> value_set;
        for (const auto& value : values) {
            value_set.insert(value);
        }
        filter.dim_sets[resolved_dim] = std::move(value_set);
    }
    if (predicate.time_range.has_value()) {
        filter.time_range = predicate.time_range;
    }
    if (predicate.min_duration_us.has_value()) {
        filter.min_duration_us = predicate.min_duration_us.value();
    }
    if (predicate.max_duration_us.has_value()) {
        filter.max_duration_us = predicate.max_duration_us.value();
    }
    return filter;
}

bool matches_predicate(const JsonValue& json, const PredicateFilter& filter) {
    for (const auto& [dim, values] : filter.dim_sets) {
        std::string event_value;
        if (dim == "name") {
            event_value = json["name"].get<std::string>();
        } else if (dim == "cat") {
            event_value = json["cat"].get<std::string>();
        } else if (dim == "pid") {
            event_value = std::to_string(json["pid"].get<std::uint64_t>());
        } else if (dim == "tid") {
            event_value = std::to_string(json["tid"].get<std::uint64_t>());
        } else if (dim == "hhash") {
            auto args = json["args"];
            if (!args.exists()) return false;
            event_value = args["hhash"].get<std::string>();
        } else if (dim == "fhash") {
            auto args = json["args"];
            if (!args.exists()) return false;
            auto fh = args["fhash"];
            if (!fh.exists()) return false;
            event_value = fh.get<std::string>();
        } else if (dim == "shash") {
            auto args = json["args"];
            if (!args.exists()) return false;
            auto sh = args["cmd_hash"];
            if (!sh.exists()) return false;
            event_value = sh.get<std::string>();
        } else {
            continue;
        }
        if (values.find(event_value) == values.end()) {
            return false;
        }
    }
    if (filter.time_range) {
        double ts = static_cast<double>(json["ts"].get<std::uint64_t>());
        if (ts < filter.time_range->first || ts > filter.time_range->second) {
            return false;
        }
    }
    if (filter.min_duration_us || filter.max_duration_us) {
        double dur = static_cast<double>(json["dur"].get<std::uint64_t>());
        if (filter.min_duration_us && dur < *filter.min_duration_us) {
            return false;
        }
        if (filter.max_duration_us && dur > *filter.max_duration_us) {
            return false;
        }
    }
    return true;
}

bool matches_any_predicate(const JsonValue& json,
                           const std::vector<PredicateFilter>& filters) {
    if (filters.empty()) return true;
    for (const auto& filter : filters) {
        if (matches_predicate(json, filter)) return true;
    }
    return false;
}

bool metadata_matches_identity(const JsonValue& json,
                               const std::vector<PredicateFilter>& filters) {
    if (filters.empty()) return true;
    for (const auto& filter : filters) {
        bool all_match = true;
        for (const auto& [dim, values] : filter.dim_sets) {
            if (dim != "pid" && dim != "tid") continue;
            std::string event_value;
            if (dim == "pid") {
                event_value = std::to_string(json["pid"].get<std::uint64_t>());
            } else {
                event_value = std::to_string(json["tid"].get<std::uint64_t>());
            }
            if (values.find(event_value) == values.end()) {
                all_match = false;
                break;
            }
        }
        if (all_match) return true;
    }
    return false;
}

}  // namespace dftracer::utils::utilities::composites::dft::views
