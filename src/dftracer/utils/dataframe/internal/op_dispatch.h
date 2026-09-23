#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_OP_DISPATCH_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_OP_DISPATCH_H

#include <dftracer/utils/dataframe/abi.h>

namespace dftracer::utils::dataframe::internal {

using CS = const dftu_series*;
using CDF = const dftu_dataframe*;
using DF = dftu_dataframe*;
using CLF = const dftu_lazyframe*;
using LF = dftu_lazyframe*;

// Maps a packed op signature to the function-pointer type a registered fn of
// that shape must have. Left undefined for a signature dftu_op_run/
// dftu_op_run_aggregate/dftu_op_run_frame has no case for, so a .def row of an
// unhandled shape fails to compile instead of silently returning nullptr
// forever at run time.
template <dftu_op_sig S>
struct op_fn;

#define DFTU_OP_FN(sig, FnType) \
    template <>                 \
    struct op_fn<sig> {         \
        using type = FnType;    \
    };

// dftu_op_run (DFTU_OP_KIND_SERIES).
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, NONE, NONE), dftu_series* (*)(CS))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, SERIES, NONE), dftu_series* (*)(CS, CS))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, SERIES, SERIES),
           dftu_series* (*)(CS, CS, CS))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, SCALAR, NONE),
           dftu_series* (*)(CS, dftu_scalar))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, SCALAR, I32),
           dftu_series* (*)(CS, dftu_scalar, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, CMP, SCALAR),
           dftu_series* (*)(CS, dftu_cmp_op, dftu_scalar))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, PRIM, NONE),
           dftu_series* (*)(CS, dftu_prim_op))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, SERIES, LOGICAL),
           dftu_series* (*)(CS, CS, dftu_logical_op))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, SERIES, CMP),
           dftu_series* (*)(CS, CS, dftu_cmp_op))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, DTYPE, NONE),
           dftu_series* (*)(CS, dftu_dtype))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, STR, NONE),
           dftu_series* (*)(CS, const char*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, STR, STR),
           dftu_series* (*)(CS, const char*, int32_t, const char*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, STR, I64),
           dftu_series* (*)(CS, const char*, int32_t, int64_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, I64, I64),
           dftu_series* (*)(CS, int64_t, int64_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, I64, CHAR),
           dftu_series* (*)(CS, int64_t, char))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, I64, NONE),
           dftu_series* (*)(CS, int64_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, SCALAR, SCALAR),
           dftu_series* (*)(CS, dftu_scalar, dftu_scalar))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, I32, NONE),
           dftu_series* (*)(CS, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, F64, NONE), dftu_series* (*)(CS, double))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, I64, F64),
           dftu_series* (*)(CS, int64_t, double))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, STR, I32),
           dftu_series* (*)(CS, const char*, int32_t, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, I32, I32),
           dftu_series* (*)(CS, int32_t, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, I64, I32),
           dftu_series* (*)(CS, int64_t, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, I64, ROLLING),
           dftu_series* (*)(CS, int64_t, dftu_rolling_op))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, STR, NONE, NONE),
           dftu_series* (*)(const char*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, STR, STR, NONE),
           dftu_series* (*)(const char*, int32_t, const char*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, RANK, I64),
           dftu_series* (*)(CS, dftu_rank_method, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, I64, U64),
           dftu_series* (*)(CS, int64_t, uint64_t))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, SERIES, I64LIST, NONE),
           dftu_series* (*)(CS, const int64_t*, int32_t))
// A frame-shaped SERIES-return op: the frame is not the primary operand of a
// FRAME-kind op, so it rides args[0].frame rather than an in[]/frames[] array.
DFTU_OP_FN(DFTU_OP_SIG(SERIES, FRAME, NONE, NONE), dftu_series* (*)(CDF))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, FRAME, QUERY, NONE),
           dftu_series* (*)(CDF, const dftu_query*))
DFTU_OP_FN(DFTU_OP_SIG(SERIES, FRAME, STRLIST, I64),
           dftu_series* (*)(CDF, const char* const*, int32_t, int64_t))

// dftu_op_run_aggregate (DFTU_OP_KIND_AGGREGATE).
DFTU_OP_FN(DFTU_OP_SIG(SCALAR, SERIES, NONE, NONE), dftu_scalar (*)(CS))
DFTU_OP_FN(DFTU_OP_SIG(SCALAR, SERIES, REDUCE, NONE),
           dftu_scalar (*)(CS, dftu_reduce_op))
DFTU_OP_FN(DFTU_OP_SIG(SCALAR, SERIES, SERIES, NONE), dftu_scalar (*)(CS, CS))
DFTU_OP_FN(DFTU_OP_SIG(I64, SERIES, NONE, NONE), int64_t (*)(CS))
DFTU_OP_FN(DFTU_OP_SIG(BOOL, SERIES, NONE, NONE), int32_t (*)(CS))
DFTU_OP_FN(DFTU_OP_SIG(BOOL, SERIES, I32, NONE), int32_t (*)(CS, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(F64, SERIES, NONE, NONE), double (*)(CS))
DFTU_OP_FN(DFTU_OP_SIG(F64, SERIES, I32, NONE), double (*)(CS, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(BOOL, STR, STR, NONE),
           int32_t (*)(const char*, int32_t, const char*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(F64, SERIES, F64, NONE), double (*)(CS, double))

// dftu_op_run_frame (DFTU_OP_KIND_FRAME).
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, NONE, NONE), DF (*)(CDF))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, I64, NONE), DF (*)(CDF, int64_t))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, I64, I64), DF (*)(CDF, int64_t, int64_t))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, SCALAR, NONE), DF (*)(CDF, dftu_scalar))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, SERIES, NONE), DF (*)(CDF, CS))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, STR, NONE), DF (*)(CDF, const char*))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, STR, I32),
           DF (*)(CDF, const char*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, I32, NONE), DF (*)(CDF, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, STR, SERIES), DF (*)(CDF, const char*, CS))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, STRLIST, NONE),
           DF (*)(CDF, const char* const*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, STRLIST, I32),
           DF (*)(CDF, const char* const*, int32_t, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, STRLIST, STRLIST),
           DF (*)(CDF, const char* const*, int32_t, const char* const*,
                  int32_t))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, STRLIST, I32LIST),
           DF (*)(CDF, const char* const*, int32_t, const int32_t*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, STRLIST, AGGLIST),
           DF (*)(CDF, const char* const*, int32_t, const dftu_group_agg*,
                  int32_t))
DFTU_OP_FN(DFTU_OP_SIG6(FRAME, FRAME, STR, I64, I32, NONE),
           DF (*)(CDF, const char*, int64_t, int32_t))
DFTU_OP_FN(DFTU_OP_SIG6(FRAME, FRAME, STR, STR, STR, STR),
           DF (*)(CDF, const char*, const char*, const char*, const char*))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, SERIES, NONE, NONE), DF (*)(CS))
DFTU_OP_FN(DFTU_OP_SIG6(FRAME, FRAME, I64LIST, NONE, NONE, NONE),
           DF (*)(CDF, const int64_t*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG6(FRAME, FRAME, I64, U64, NONE, NONE),
           DF (*)(CDF, int64_t, uint64_t))
DFTU_OP_FN(DFTU_OP_SIG6(FRAME, FRAME, STR, I64, I64, AGGLIST),
           DF (*)(CDF, const char*, int64_t, int64_t, const dftu_group_agg*,
                  int32_t))
DFTU_OP_FN(DFTU_OP_SIG8(FRAME, FRAME, STR, STR, STR, SCALAR, SCALAR, STR),
           DF (*)(CDF, const char*, const char*, const char*, dftu_scalar,
                  dftu_scalar, const char*))
DFTU_OP_FN(DFTU_OP_SIG8(FRAME, FRAME, STRLIST, I32, I64, I32, I32, NONE),
           DF (*)(CDF, const char* const*, int32_t, int32_t, int64_t, int32_t,
                  int32_t))

DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, FRAME, NONE), DF (*)(CDF, CDF))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, FRAME, I32),
           DF (*)(CDF, CDF, dftu_concat_how))
DFTU_OP_FN(DFTU_OP_SIG(FRAME, FRAME, FRAME, I64), DF (*)(CDF, CDF, int64_t))
// The utilities library's relational kernels (registered at load, not .def
// rows); the runner still needs a case per shape.
DFTU_OP_FN(DFTU_OP_SIG8(FRAME, FRAME, STRLIST, STRLIST, WINLIST, NONE, NONE,
                        NONE),
           DF (*)(CDF, const char* const*, int32_t, const char* const*, int32_t,
                  const dftu_window_spec*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG8(FRAME, FRAME, STRLIST, STR, I64, STRLIST, I32, I64LIST),
           DF (*)(CDF, const char* const*, int32_t, const char*, int64_t,
                  const char* const*, int32_t, dftu_gap_fill_mode,
                  const int64_t*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG8(FRAME, FRAME, FRAME, STR, STRLIST, I32, I64, NONE),
           DF (*)(CDF, CDF, const char*, const char* const*, int32_t,
                  dftu_asof_direction, int64_t))
DFTU_OP_FN(DFTU_OP_SIG8(FRAME, FRAME, FRAME, STR, STR, STR, STRLIST, I32),
           DF (*)(CDF, CDF, const char*, const char*, const char*,
                  const char* const*, int32_t, int32_t))
DFTU_OP_FN(DFTU_OP_SIG8(FRAME, FRAME, FRAME, STRLIST, STRLIST, I32, STR, NONE),
           DF (*)(CDF, CDF, const char* const*, const char* const*, int32_t,
                  dftu_join_how, const char*))

// dftu_op_run_lazy (DFTU_OP_KIND_LAZY).
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, NONE, NONE), LF (*)(CLF))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, STR, NONE), LF (*)(CLF, const char*))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, SCALAR, NONE), LF (*)(CLF, dftu_scalar))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, EXPR, NONE), LF (*)(CLF, const dftu_expr*))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, STR, EXPR),
           LF (*)(CLF, const char*, const dftu_expr*))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, I64, NONE), LF (*)(CLF, int64_t))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, I64, I64), LF (*)(CLF, int64_t, int64_t))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, U64, NONE), LF (*)(CLF, uint64_t))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, I64, U64), LF (*)(CLF, int64_t, uint64_t))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, STRLIST, NONE),
           LF (*)(CLF, const char* const*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, STRLIST, STRLIST),
           LF (*)(CLF, const char* const*, int32_t, const char* const*,
                  int32_t))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, STRLIST, AGGLIST),
           LF (*)(CLF, const char* const*, int32_t, const dftu_group_agg*,
                  int32_t))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, STR, I32), LF (*)(CLF, const char*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, SERIES, NONE), LF (*)(CLF, CS))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, STRLIST, I32),
           LF (*)(CLF, const char* const*, int32_t, int32_t))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, I32, NONE), LF (*)(CLF, int32_t))
DFTU_OP_FN(DFTU_OP_SIG6(LAZY, LAZY, STR, I64, I32, NONE),
           LF (*)(CLF, const char*, int64_t, int32_t))
DFTU_OP_FN(DFTU_OP_SIG6(LAZY, LAZY, STR, STR, STR, STR),
           LF (*)(CLF, const char*, const char*, const char*, const char*))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, I64LIST, NONE),
           LF (*)(CLF, const int64_t*, int32_t))
DFTU_OP_FN(DFTU_OP_SIG8(LAZY, LAZY, STR, I64, I64, AGGLIST, I64, I32),
           LF (*)(CLF, const char*, int64_t, int64_t, const dftu_group_agg*,
                  int32_t, int64_t, int32_t))
DFTU_OP_FN(DFTU_OP_SIG8(LAZY, LAZY, STRLIST, I32, I64, I32, I32, NONE),
           LF (*)(CLF, const char* const*, int32_t, int32_t, int64_t, int32_t,
                  int32_t))

DFTU_OP_FN(DFTU_OP_SIG8(LAZY, LAZY, LAZY, STRLIST, STRLIST, I32, STR, NONE),
           LF (*)(CLF, CLF, const char* const*, const char* const*, int32_t,
                  dftu_join_how, const char*))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, LAZY, NONE), LF (*)(CLF, CLF))
DFTU_OP_FN(DFTU_OP_SIG(LAZY, LAZY, LAZY, I64), LF (*)(CLF, CLF, int64_t))

#undef DFTU_OP_FN

}  // namespace dftracer::utils::dataframe::internal

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_OP_DISPATCH_H
