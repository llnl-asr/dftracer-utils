#ifndef DFTRACER_UTILS_INDEX_PLAN_RESOLVED_FIELD_REWRITER_H
#define DFTRACER_UTILS_INDEX_PLAN_RESOLVED_FIELD_REWRITER_H

#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/query/query.h>

#include <optional>
#include <string>

namespace dftracer::utils::index::plan {

/// Rewrites every leaf on a `resolved.<key field>.<field>` column into
/// `<key field> in [keys]`: the keys of the rows of the schema's dictionary
/// whose field satisfies the leaf. Returns nullopt when the query names no
/// resolved column. Throws DFTUtilsException INVALID_ARGUMENT for a name the
/// schema does not resolve.
std::optional<query::Query> rewrite_resolved_fields(
    const query::Query& query, const index::store::IndexDatabase& db,
    const RecordSchema& schema);

/// rewrite_resolved_fields for `trace` in the index at `index_path`, with the
/// schema `trace` was indexed with (detected from the file when the index
/// has no record of it).
std::optional<query::Query> rewrite_resolved_fields(
    const query::Query& query, const std::string& index_path,
    const std::string& trace);

/// True if the query names a `resolved.` column. No DB access.
bool has_resolved_fields(const query::Query& query);

/// The schema `trace` was indexed with in `db`; null when `db` has no
/// record of it.
const RecordSchema* recorded_schema(const index::store::IndexDatabase& db,
                                    const std::string& trace);

}  // namespace dftracer::utils::index::plan

#endif  // DFTRACER_UTILS_INDEX_PLAN_RESOLVED_FIELD_REWRITER_H
