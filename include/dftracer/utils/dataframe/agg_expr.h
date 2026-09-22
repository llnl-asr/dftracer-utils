#ifndef DFTRACER_UTILS_DATAFRAME_AGG_EXPR_H
#define DFTRACER_UTILS_DATAFRAME_AGG_EXPR_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/agg_op_codes.h>
#include <dftracer/utils/dataframe/expr.h>

#include <string>
#include <vector>

// Aggregation over expressions: the group key and every aggregate's value are
// expr-IR expressions, not pre-materialized columns. The key and all value
// expressions compile into ONE program - so a subexpression shared across
// aggregates (e.g. `a+b` in both sum(a+b) and var(a+b)) is computed once (CSE),
// and only the referenced input columns are materialized (the pruner) - then
// the evaluated columns fold through the FieldStat aggregation engine.
namespace dftracer::utils::dataframe {

/// One aggregate whose value is an expression over the input columns. `value`
/// is ignored for Count (the group row count).
struct AggExprSpec {
    AggOp op = AggOp::Count;
    Expr value;
    std::string out;
    double param = 0.0;  ///< Pct: the quantile level q in [0, 1]
    Expr by{};           ///< ArgMax: the value maximized (value is the field
                         ///< represented); unused otherwise

    /// Rename the output column (fluent), e.g. `agg_sum(a + b).as("sum_ab")`.
    AggExprSpec as(std::string name) const {
        AggExprSpec s = *this;
        s.out = std::move(name);
        return s;
    }
};

/// Ergonomic aggregate builders over an expression value. `out` defaults to the
/// op name; chain `.as("name")` to rename. The C ABI (`dftu_agg_*`) and the
/// Python DSL (`F.x.sum()`) mirror these, so every frontend builds the same
/// spec. Feed the result to group_agg_expr.
AggExprSpec agg_count(std::string out = "count");
AggExprSpec agg_sum(Expr value, std::string out = "sum");
AggExprSpec agg_min(Expr value, std::string out = "min");
AggExprSpec agg_max(Expr value, std::string out = "max");
AggExprSpec agg_mean(Expr value, std::string out = "mean");
AggExprSpec agg_var(Expr value, std::string out = "var");
AggExprSpec agg_std(Expr value, std::string out = "std");
AggExprSpec agg_skew(Expr value, std::string out = "skew");
AggExprSpec agg_kurt(Expr value, std::string out = "kurt");
/// A DDSketch quantile at level `q` in [0, 1] (mergeable, order-independent).
AggExprSpec agg_pct(Expr value, double q, std::string out = "pct");
/// The DDSketch histogram of `value`, a list<struct{lo,hi,count}> per group.
AggExprSpec agg_hist(Expr value, std::string out = "hist");
/// Sum of squares of `value` (Float64), from the shared FieldStat.
AggExprSpec agg_sumsq(Expr value, std::string out = "sumsq");
/// The String repr of `value` at the row maximizing `by`, per group.
AggExprSpec agg_argmax(Expr value, Expr by, std::string out = "argmax");
/// Distinct String values of `value`, sorted and joined (see AggOp::SetUnion).
AggExprSpec agg_set_union(Expr value, std::string out = "set_union");

/// Group `inputs` by `keys` (N expressions) and compute each spec, evaluating
/// the keys and values in one fused, CSE'd, pruned pass. Identical value
/// expressions share a single evaluated column (and thus one accumulator). A
/// bare column-ref key shares that input column directly (any type); a
/// computed key routes through the numeric expr evaluator. The result is one
/// key column per `key_names` (in order) plus one column per spec.
DataFrame group_agg_expr(const std::vector<Expr>& keys,
                         const std::vector<AggExprSpec>& specs,
                         const std::vector<const Series*>& inputs,
                         const std::vector<std::string>& key_names);
/// Single-key convenience: forwards to the N-key form.
DataFrame group_agg_expr(const Expr& key, const std::vector<AggExprSpec>& specs,
                         const std::vector<const Series*>& inputs,
                         const std::string& key_name);

}  // namespace dftracer::utils::dataframe

// C ABI: build aggregate-over-expression specs and run a grouped aggregation
// from any C/C++ consumer. Op codes mirror dataframe::AggOp; the compile (CSE
// across value expressions) + pruning happen inside
// dftu_dataframe_group_agg_expr, so every consumer gets the same engine. Value
// expressions are borrowed (not freed).
extern "C" {

/** One aggregate: `op` is a DFTU_AGG_* code, `value` the value expression
 * (borrowed; NULL for COUNT), `out` the result column name (borrowed), `param`
 * the quantile level for DFTU_AGG_PCT (0 otherwise), `by` the value maximized
 * for DFTU_AGG_ARGMAX (borrowed; NULL otherwise). */
typedef struct dftu_agg_spec {
    int32_t op;
    const dftu_expr* value;
    const char* out;
    double param;
    const dftu_expr* by;
} dftu_agg_spec;

dftu_agg_spec dftu_agg_count(const char* out);
dftu_agg_spec dftu_agg_sum(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_min(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_max(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_mean(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_var(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_std(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_skew(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_kurt(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_first(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_last(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_pct(const dftu_expr* value, double q, const char* out);
dftu_agg_spec dftu_agg_hist(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_sumsq(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_argmax(const dftu_expr* value, const dftu_expr* by,
                              const char* out);
dftu_agg_spec dftu_agg_set_union(const dftu_expr* value, const char* out);

/** Group `n_inputs` columns by `key` (an expression; a bare column ref, e.g. a
 * string category, is taken directly) and compute each of `n_specs` aggregates.
 * The value expressions compile into one program (CSE) and only referenced
 * inputs are materialized (pruner). Writes the key column to `*out_key` and one
 * owned column per spec into `out_values[0..n_specs)`. Returns n_specs, or -1
 * on error (writing nothing). */
int32_t dftu_dataframe_group_agg_expr(const dftu_expr* key,
                                      const dftu_agg_spec* specs,
                                      int32_t n_specs,
                                      const dftu_series* const* inputs,
                                      int32_t n_inputs, dftu_series** out_key,
                                      dftu_series** out_values);
}

#endif  // DFTRACER_UTILS_DATAFRAME_AGG_EXPR_H
