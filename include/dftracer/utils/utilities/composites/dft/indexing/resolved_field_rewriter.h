#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_RESOLVED_FIELD_REWRITER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_RESOLVED_FIELD_REWRITER_H

#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/indexer/index_database.h>

#include <optional>

namespace dftracer::utils::utilities::composites::dft::indexing {

/// Virtual query fields that resolve a human-readable value to its hash before
/// scanning. `resolved.<name>` and the short form `r.<name>` are equivalent:
///   resolved.fpath / r.fpath / resolved.cwd  -> fhash
///   resolved.hostname / r.hostname           -> hhash
///   resolved.exec / resolved.cmd             -> shash
///
/// Predicates on these fields (==, !=, in, not in, like/ilike, ~/~*, substring)
/// are rewritten into concrete `<hash> in [...]` / `not in [...]` clauses using
/// the index hash tables, so the existing pruner and per-event evaluator handle
/// them unchanged. Pattern operators enumerate the hash table and match each
/// resolved name; exact operators use a direct reverse lookup.
///
/// Returns the rewritten query, or nullopt if it references no virtual fields
/// (in which case the caller keeps the original query untouched).
std::optional<common::query::Query> rewrite_resolved_fields(
    const common::query::Query& query, const indexer::IndexDatabase& db);

/// True if the query references any resolved.*/r.* virtual field. Cheap AST
/// walk with no DB access; use to decide whether a rewrite is needed.
bool has_resolved_fields(const common::query::Query& query);

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_RESOLVED_FIELD_REWRITER_H
