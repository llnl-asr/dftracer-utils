#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_PREDICATE_FILTER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_PREDICATE_FILTER_H

#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::views {

struct PredicateFilter {
    std::unordered_map<std::string, std::unordered_set<std::string>> dim_sets;
    std::optional<std::pair<double, double>> time_range;
    std::optional<double> min_duration_us;
    std::optional<double> max_duration_us;
};

PredicateFilter build_predicate_filter(const ViewPredicate& predicate);

bool matches_predicate(const common::json::JsonValue& json,
                       const PredicateFilter& filter);

bool matches_any_predicate(const common::json::JsonValue& json,
                           const std::vector<PredicateFilter>& filters);

bool metadata_matches_identity(const common::json::JsonValue& json,
                               const std::vector<PredicateFilter>& filters);

}  // namespace dftracer::utils::utilities::composites::dft::views

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_PREDICATE_FILTER_H
