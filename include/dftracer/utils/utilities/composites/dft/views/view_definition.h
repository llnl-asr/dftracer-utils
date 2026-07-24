#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_DEFINITION_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_DEFINITION_H

#include <dftracer/utils/utilities/common/query/query.h>

#include <optional>
#include <string>

namespace dftracer::utils::utilities::composites::dft::views {

using dftracer::utils::utilities::common::query::Query;

/// Named view definition with optional query filter.
struct ViewDefinition {
    std::string name;              ///< View name.
    std::string description;       ///< Human-readable description.
    std::optional<Query> query;    ///< Event filter (nullopt = match all).
    bool include_metadata = true;  ///< Include ph=M metadata events.
    /// Emit hash metadata (FH/HH/SH) immediately instead of buffering it for
    /// reference-driven flushing. Consumers that aggregate all metadata (the
    /// activity-summary build) need this, since traces whose data events carry
    /// no hash args would otherwise never see it.
    bool emit_all_metadata = false;

    ViewDefinition& with_name(const std::string& n);
    ViewDefinition& with_description(const std::string& d);
    /// Set query from a DSL string. Silently ignored if parse fails.
    ViewDefinition& with_query(const std::string& query_str);
    ViewDefinition& with_query(Query q);
    ViewDefinition& with_include_metadata(bool v);
    ViewDefinition& with_emit_all_metadata(bool v);

    std::string to_json() const;
    static ViewDefinition from_json(const std::string& json);

    /// POSIX/STDIO I/O operations.
    static ViewDefinition io_view();
    /// AI/HPC compute and framework operations.
    static ViewDefinition compute_view();
    /// DLIO benchmark operations.
    static ViewDefinition dlio_view();
};

}  // namespace dftracer::utils::utilities::composites::dft::views

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_DEFINITION_H
