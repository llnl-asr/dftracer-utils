// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_AGG_EXPR_H
#define DFTRACER_UTILS_DATAFRAME_AGG_EXPR_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/agg_op_codes.h>
#include <dftracer/utils/dataframe/c_api_agg_expr.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

// Aggregation over expressions: the group key and every aggregate's value are
// expr-IR expressions, not pre-materialized columns. The key and all value
// expressions compile into ONE program - so a subexpression shared across
// aggregates (e.g. `a+b` in both sum(a+b) and var(a+b)) is computed once (CSE),
// and only the referenced input columns are materialized (the pruner) - then
// the evaluated columns fold through the FieldStat aggregation engine.
namespace dftracer::utils::dataframe {

/// One aggregate whose value is an expression over the input columns. `value`
/// is ignored for Count (the group row count); CountValid counts its non-null
/// values.
struct AggExprSpec {
    AggOp op = AggOp::Count;
    Expr value;
    dstd::String out;
    double param = 0.0;  ///< Pct: the quantile level q in [0, 1]; SetUnion:
                         ///< nonzero asks for the typed list
    Expr by{};           ///< ArgMax: the value maximized (value is the field
                         ///< represented); unused otherwise
    dstd::String fn{};   ///< Udaf: the registered name of the aggregate
    dstd::Vector<Expr> args{};  ///< Udaf: the arguments after `value`, which
                                ///< is argument 0 (unset for no argument)

    /// Rename the output column (fluent), e.g. `agg_sum(a + b).as("sum_ab")`.
    AggExprSpec as(dstd::String name) const {
        AggExprSpec s = *this;
        s.out = std::move(name);
        return s;
    }
};

/// Ergonomic aggregate builders over an expression value. `out` defaults to the
/// op name; chain `.as("name")` to rename. The C ABI (`dftu_agg_*`) and the
/// Python DSL (`F.x.sum()`) mirror these, so every frontend builds the same
/// spec. Feed the result to group_agg_expr.
AggExprSpec agg_count(dstd::String out = "count");
/// The number of non-null values of `value` per group (`count` with a column).
AggExprSpec agg_count_valid(Expr value, dstd::String out = "count_valid");
AggExprSpec agg_sum(Expr value, dstd::String out = "sum");
AggExprSpec agg_min(Expr value, dstd::String out = "min");
AggExprSpec agg_max(Expr value, dstd::String out = "max");
AggExprSpec agg_mean(Expr value, dstd::String out = "mean");
AggExprSpec agg_var(Expr value, dstd::String out = "var");
AggExprSpec agg_std(Expr value, dstd::String out = "std");
AggExprSpec agg_skew(Expr value, dstd::String out = "skew");
AggExprSpec agg_kurt(Expr value, dstd::String out = "kurt");
/// A DDSketch quantile at level `q` in [0, 1] (mergeable, order-independent).
AggExprSpec agg_pct(Expr value, double q, dstd::String out = "pct");
/// The DDSketch histogram of `value`, a list<struct{lo,hi,count}> per group.
AggExprSpec agg_hist(Expr value, dstd::String out = "hist");
/// Sum of squares of `value` (Float64), from the shared @ref FieldStat.
AggExprSpec agg_sumsq(Expr value, dstd::String out = "sumsq");
/// The String repr of `value` at the row maximizing `by`, per group.
AggExprSpec agg_argmax(Expr value, Expr by, dstd::String out = "argmax");
/// Distinct String values of `value`, sorted and joined (see AggOp::SetUnion).
/// With `typed`, a list of the distinct non-null values in their own type
/// (string, bool, int64, uint64 or float64), ascending, no separator limit.
AggExprSpec agg_set_union(Expr value, dstd::String out = "set_union",
                          bool typed = false);

/// Group `inputs` by `keys` (N expressions) and compute each spec, evaluating
/// the keys and values in one fused, CSE'd, pruned pass. Identical value
/// expressions share a single evaluated column (and thus one accumulator). A
/// bare column-ref key shares that input column directly (any type); a
/// computed key routes through the numeric expr evaluator. The result is one
/// key column per `key_names` (in order) plus one column per spec. A spec that
/// needs a bare column reference or a `by` column and lacks it, a value
/// expression that is not numeric, and any @ref eval or @ref group_agg error
/// come back as an error result.
dstd::Result<DataFrame> group_agg_expr(
    const dstd::Vector<Expr>& keys, const dstd::Vector<AggExprSpec>& specs,
    const dstd::Vector<const Series*>& inputs,
    const dstd::Vector<dstd::String>& key_names);
/// Single-key convenience: forwards to the N-key form.
dstd::Result<DataFrame> group_agg_expr(
    const Expr& key, const dstd::Vector<AggExprSpec>& specs,
    const dstd::Vector<const Series*>& inputs, const dstd::String& key_name);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_AGG_EXPR_H
