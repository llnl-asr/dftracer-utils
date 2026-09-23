#ifndef DFTRACER_UTILS_DATAFRAME_AGG_OP_CODES_H
#define DFTRACER_UTILS_DATAFRAME_AGG_OP_CODES_H

/* Stable aggregate op codes shared by the dataframe C ABI (dftu_agg_spec) and
   the plugin ABI (dftu_agg_col), so a plugin author selects an aggregate by a
   named enum, not a bare int. The values mirror dataframe::AggOp; a
   static_assert in agg.cpp keeps the two definitions in lockstep. The C seams
   still carry the code as a fixed-width int for ABI stability and range-check
   it at the boundary. */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DFTU_AGG_COUNT = 0,
    DFTU_AGG_SUM = 1,
    DFTU_AGG_MIN = 2,
    DFTU_AGG_MAX = 3,
    DFTU_AGG_MEAN = 4,
    DFTU_AGG_VAR = 5,
    DFTU_AGG_STD = 6,
    DFTU_AGG_SKEW = 7,
    DFTU_AGG_KURT = 8,
    DFTU_AGG_FIRST = 9,
    DFTU_AGG_LAST = 10,
    DFTU_AGG_PCT = 11,
    DFTU_AGG_HIST = 12,
    DFTU_AGG_ARGMAX = 13,
    DFTU_AGG_SUMSQ = 14,
    DFTU_AGG_SET_UNION = 15,
    DFTU_AGG_BUSY = 16,
    DFTU_AGG_CONCURRENCY = 17,
    DFTU_AGG_UTILIZATION = 18,
    DFTU_AGG_ACTIVE = 19,
    DFTU_AGG_COUNT_VALID = 20,
    DFTU_AGG_ARGMIN = 21,
    DFTU_AGG_BIT_OR = 22,
    DFTU_AGG_DISTINCT = 23,
    DFTU_AGG_LIST_SORTED = 24,
    DFTU_AGG_TOPK = 25,
    DFTU_AGG_BOTTOMK = 26,
    DFTU_AGG_APPROX_TOPK = 27,
    DFTU_AGG_SAMPLE = 28,
    DFTU_AGG_CORR = 29,
    DFTU_AGG_COVAR_POP = 30,
    DFTU_AGG_COVAR_SAMP = 31,
    DFTU_AGG_REGR_SLOPE = 32,
    DFTU_AGG_REGR_INTERCEPT = 33,
    DFTU_AGG_REGR_R2 = 34,
    DFTU_AGG_PROD = 35
} dftu_agg_op;

#ifdef __cplusplus
}
#endif

#endif  // DFTRACER_UTILS_DATAFRAME_AGG_OP_CODES_H
