#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_DEFINITION_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_DEFINITION_H

#include <dftracer/utils/utilities/common/query/query.h>

#include <optional>
#include <string>

namespace dftracer::utils::utilities::composites::dft::views {

using dftracer::utils::utilities::common::query::Query;

struct ViewDefinition {
    std::string name;
    std::string description;
    std::optional<Query> query;
    bool include_metadata = true;

    ViewDefinition& with_name(const std::string& n);
    ViewDefinition& with_description(const std::string& d);
    ViewDefinition& with_query(const std::string& query_str);
    ViewDefinition& with_query(Query q);
    ViewDefinition& with_include_metadata(bool v);

    std::string to_json() const;
    static ViewDefinition from_json(const std::string& json);

    static ViewDefinition io_view();
    static ViewDefinition compute_view();
    static ViewDefinition dlio_view();
};

}  // namespace dftracer::utils::utilities::composites::dft::views

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_DEFINITION_H
