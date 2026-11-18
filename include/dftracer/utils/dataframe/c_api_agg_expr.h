// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_C_API_AGG_EXPR_H
#define DFTRACER_UTILS_DATAFRAME_C_API_AGG_EXPR_H

#include <dftracer/utils/dataframe/c_api.h>
#include <stdint.h>

// C ABI: build aggregate-over-expression specs and run a grouped aggregation
// from any C/C++ consumer. Op codes mirror dataframe::AggOp; the compile (CSE
// across value expressions) + pruning happen inside
// dftu_dataframe_group_agg_expr, so every consumer gets the same engine. Value
// expressions are borrowed (not freed).
#ifdef __cplusplus
extern "C" {
#endif

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
dftu_agg_spec dftu_agg_count_valid(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_sum(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_min(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_max(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_mean(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_var(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_std(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_skew(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_kurt(const dftu_expr* value, const char* out);
/** The pandas sample skewness / sample excess kurtosis of `value` (NaN below
 * three / four present values and for a constant column). */
dftu_agg_spec dftu_agg_skew_unbiased(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_kurt_unbiased(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_first(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_last(const dftu_expr* value, const char* out);
/** The approximate DDSketch quantile at level `q` in [0, 1]. */
dftu_agg_spec dftu_agg_pct(const dftu_expr* value, double q, const char* out);
/** The exact quantile at level `q` in [0, 1] (`interp` a dftu_interpolation
 * from dataframe/c_api.h); null for a group with no present value. The group
 * state keeps every present value, 8 bytes each. An `interp` outside the enum
 * is read as DFTU_INTERP_LINEAR. */
dftu_agg_spec dftu_agg_quantile(const dftu_expr* value, double q,
                                int32_t interp, const char* out);
dftu_agg_spec dftu_agg_hist(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_sumsq(const dftu_expr* value, const char* out);
dftu_agg_spec dftu_agg_argmax(const dftu_expr* value, const dftu_expr* by,
                              const char* out);
dftu_agg_spec dftu_agg_set_union(const dftu_expr* value, const char* out,
                                 int32_t typed);

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
#ifdef __cplusplus
}
#endif

#endif  // DFTRACER_UTILS_DATAFRAME_C_API_AGG_EXPR_H
