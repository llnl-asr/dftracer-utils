#ifndef DFTRACER_UTILS_DUQL_SUBSUMPTION_H
#define DFTRACER_UTILS_DUQL_SUBSUMPTION_H

#include <dftracer/utils/duql/ast.h>

namespace dftracer::utils::duql {

/**
 * @brief Prove that every event matching `query` also matches `mv`, i.e.
 * rows(query) is a subset of rows(mv).
 *
 * This is the predicate-subsumption test behind materialized-view rewriting: an
 * MV filtered by `mv` can answer `query` (by re-applying `query` on the smaller
 * result) exactly when it holds a superset of the query's rows.
 *
 * The check is the Goldstein-Larson conjunctive method restricted to a
 * join-free event stream: each predicate is flattened to a conjunction of typed
 * atoms bucketed as numeric intervals, value sets (`==`/`in`), and opaque
 * residuals (`like`/regex/`!=`/`not in`/`not (...)`/top-level `or`). `mv`
 * subsumes `query` iff, per field, the query is at-least-as-constrained as `mv`
 * on every interval and set, and every residual of `mv` also appears in
 * `query`. Anything the query constrains beyond `mv` is the compensation the
 * caller re-applies.
 *
 * SOUND but intentionally INCOMPLETE: it returns true only when subsumption is
 * provable and false when it cannot decide (e.g. cross-bucket interval-vs-set
 * reasoning, or an `or` broadened across disjuncts). A false result means "fall
 * back to a full scan", never a wrong answer. Negation is never decomposed
 * (the evaluator is 2-valued with missing-field -> false, so De Morgan is
 * unsound), so every `not (...)` stays an opaque residual.
 */
bool query_subsumes(const QueryNode& mv, const QueryNode& query);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_SUBSUMPTION_H
