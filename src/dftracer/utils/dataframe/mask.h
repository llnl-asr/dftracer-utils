#ifndef DFTRACER_UTILS_DATAFRAME_MASK_H
#define DFTRACER_UTILS_DATAFRAME_MASK_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/duql/ast.h>

// The post-materialization predicate backend: evaluate a query AST against an
// in-memory columnar batch as a SIMD boolean mask. One of the query language's
// physical backends (the scan-time evaluator in `query` matches events during a
// scan; this matches an already-materialized dataframe::DataFrame), living in
// `dataframe` so the `query` language stays independent of the dataframe
// engine.
namespace dftracer::utils::dataframe {

/// Evaluate `node` over `batch`, returning a Bool mask column of length
/// batch row count under three-valued logic: a missing column, a null cell or
/// a type mismatch is UNKNOWN (null), and and/or/not are Kleene. An UNKNOWN
/// row stores data 0, so a filter by the mask keeps only TRUE rows. An
/// expression leaf is evaluated row by row. Throws
/// DFTUtilsException{duql::DuqlErrc::Unsupported} for pattern match, any(),
/// ordered comparison on a string or bool column, and an expression on a
/// column that is not bool, integer, float or string.
dftracer::utils::dataframe::Series evaluate_mask(
    const dftracer::utils::duql::QueryNode& node,
    const dftracer::utils::dataframe::DataFrame& batch);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_MASK_H
