#ifndef DFTRACER_UTILS_DATAFRAME_PLAN_H
#define DFTRACER_UTILS_DATAFRAME_PLAN_H

#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/duql/ast.h>

#include <cstdint>
#include <string>
#include <vector>

// A query plan over a materialized columnar batch: the composable shape a query
// frontend (the DSL string, later SQL / a viz DSL) lowers to. Each stage maps
// to a dataframe primitive - filter, group_by, select, sort_by, head - so the
// plan is executed by the same engine every other surface uses.
namespace dftracer::utils::dataframe {

/// WHERE + GROUP BY/agg + SELECT + ORDER BY + LIMIT over a batch, applied in
/// that order. Absent stages are skipped.
struct QueryPlan {
    const dftracer::utils::duql::QueryNode* where =
        nullptr;           ///< predicate (borrowed); null = all rows
    std::string group_by;  ///< group key column; empty = no grouping
    std::vector<dftracer::utils::dataframe::GroupAgg> aggs;  ///< aggregates
    std::vector<std::string> select;  ///< projected columns; empty = all
    std::string order_by;             ///< sort column; empty = no sort
    bool descending = false;
    std::int64_t limit = -1;          ///< row cap; < 0 = unlimited
};

/// Execute `plan` over `input`, returning a new batch. The predicate is
/// evaluated as a SIMD mask (see mask.h) - a predicate with no columnar
/// lowering throws DFTUtilsException{duql::DuqlErrc::Unsupported}. Columns
/// are shared zero-copy where a stage does not gather.
dftracer::utils::dataframe::DataFrame execute(
    const QueryPlan& plan, const dftracer::utils::dataframe::DataFrame& input);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_PLAN_H
