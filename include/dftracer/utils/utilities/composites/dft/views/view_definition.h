#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_DEFINITION_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_DEFINITION_H

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::views {

struct ViewPredicate {
    std::unordered_map<std::string, std::vector<std::string>> bloom_dims;
    std::optional<std::pair<double, double>> time_range;
    std::optional<double> min_duration_us;
    std::optional<double> max_duration_us;

    // Fluent builder API
    ViewPredicate& with_bloom_dim(const std::string& dim,
                                  const std::vector<std::string>& values);
    ViewPredicate& with_time_range(double min_ts, double max_ts);
    ViewPredicate& with_min_duration(double us);
    ViewPredicate& with_max_duration(double us);

    // Utility methods
    bool has_bloom_dims() const;
    bool has_event_filters() const;
};

struct ViewDefinition {
    std::string name;
    std::string description;
    std::vector<ViewPredicate> predicates;
    bool include_metadata = true;  // pass through ph="M" events

    // Fluent builder API
    ViewDefinition& with_name(const std::string& n);
    ViewDefinition& with_description(const std::string& d);
    ViewDefinition& with_predicate(ViewPredicate pred);
    ViewDefinition& with_include_metadata(bool v);

    // Serialization
    std::string to_json() const;
    static ViewDefinition from_json(const std::string& json);

    // Predefined views
    static ViewDefinition io_view();
    static ViewDefinition compute_view();
    static ViewDefinition dlio_view();
};

// Free function to resolve bloom dimension aliases
std::string resolve_bloom_dimension(const std::string& dim);

}  // namespace dftracer::utils::utilities::composites::dft::views

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_DEFINITION_H