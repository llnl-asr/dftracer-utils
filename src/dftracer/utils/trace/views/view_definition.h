#ifndef DFTRACER_UTILS_TRACE_VIEWS_VIEW_DEFINITION_H
#define DFTRACER_UTILS_TRACE_VIEWS_VIEW_DEFINITION_H

#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/query/query.h>

#include <optional>
#include <string>
#include <utility>

namespace dftracer::utils::trace::views {

using dftracer::utils::query::Query;

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
    /// Apply `query` to ph=M metadata records too. Off by default so metadata
    /// bypasses the event query and survives for hash/rank harvesting; set only
    /// for an explicit phase("metadata") query.
    bool filter_metadata = false;
    /// Keep data records by time: those with begin <= ts < end, or, with
    /// `window_overlap`, those whose [ts, ts + dur) overlaps [begin, end).
    /// Metadata records are not windowed. nullopt keeps every record.
    std::optional<std::pair<double, double>> window;
    bool window_overlap = false;
    /// Decode records by exact JSON path (decode_record) rather than as
    /// dftracer events.
    bool by_path = false;
    /// With `by_path`, the paths to decode, sorted and unique; empty decodes
    /// every leaf.
    std::vector<std::string> paths;
    /// With `by_path`, the files' record schema (registered, so it lives for
    /// the process); decoding converts its fields and binds its roles.
    const dftracer::utils::index::RecordSchema* record_schema = nullptr;
    /// With `record_schema`, the declared field at each of `paths` (null when
    /// undeclared), aligned with `paths`.
    std::vector<const dftracer::utils::index::FieldSpec*> path_fields;
    /// The fields `window` reads, and microseconds per unit of each; empty
    /// reads "ts" and "dur" in microseconds.
    std::string time_path;
    std::string duration_path;
    double time_factor = 1;
    double duration_factor = 1;
    /// Drop lines that lack the query's byte needles before parsing them
    /// (index::plan::Prefilter). Results are the same either way.
    bool prefilter = true;

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

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_VIEW_DEFINITION_H
