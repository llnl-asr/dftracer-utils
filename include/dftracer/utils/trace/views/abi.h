#ifndef DFTRACER_UTILS_TRACE_VIEWS_ABI_H
#define DFTRACER_UTILS_TRACE_VIEWS_ABI_H

#include <dftracer/utils/core/common/export.h>
#include <dftracer/utils/core/coro/abi.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/query/abi.h>
#include <stdint.h>

/*
 * Stable C ABI for the trace View: a lazy plan over one or more indexed
 * .pfw.gz trace files. dftu_view is a PURE-PLAN value - every builder below
 * takes a `const dftu_view*` and returns a NEW owned handle, never consuming
 * or mutating the input, matching View's own const builder methods. NULL is
 * returned on a failed operation. Free every returned handle with
 * dftu_view_free.
 *
 * Deliberately NOT exposed here (available only via the C++
 * dftracer::utils::trace::views::View): time_bucket / time_bucket_min /
 * occ_cell / time_scale, materialize, rollup_root / views_root, cancel_when
 * (would need a C callback), memory_budget / auto_spill, agg_numeric_args,
 * metadata / emit_all_metadata, topk, export_json, phase, time_range. The
 * generic fold<P> / map_batches<P> / run_folds escape hatches take a
 * compile-time partial type P and cannot cross a C ABI at all; they stay
 * C++-only.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dftu_view dftu_view;

/** Build a plan over `n` trace files named by `paths`. `index_paths` may be
 * NULL, or any of its entries NULL or empty, to use the sidecar index
 * resolved beside that trace on first touch. Pure plan construction: no I/O
 * runs here. NULL if `paths` is NULL or `n` is negative. */
DFTU_EXPORT dftu_view* dftu_view_from_files(const char* const* paths,
                                            const char* const* index_paths,
                                            int32_t n);

/** Scan `dir` recursively for .pfw.gz files (sorted by path for determinism)
 * and build a plan over them. `index_path` may be NULL or empty for the
 * sidecar default beside each trace. Unlike dftu_view_from_files this touches
 * the filesystem, so it runs on `rt` (NULL = the installed default runtime)
 * and returns NULL on failure: a missing directory, or no .pfw.gz files
 * found. */
DFTU_EXPORT dftu_view* dftu_view_from_directory(const char* dir,
                                                const char* index_path,
                                                dftu_runtime* rt);

DFTU_EXPORT void dftu_view_free(dftu_view* v);

/** Keep only events matching `q`. `q` is borrowed; not consumed or freed. */
DFTU_EXPORT dftu_view* dftu_view_filter(const dftu_view* v,
                                        const dftu_query* q);

/** Project the collected result to the `n` named columns. */
DFTU_EXPORT dftu_view* dftu_view_select(const dftu_view* v,
                                        const char* const* cols, int32_t n);

/** Cap the result to `n` rows/events (0 = unlimited), applied after
 * dftu_view_offset. */
DFTU_EXPORT dftu_view* dftu_view_limit(const dftu_view* v, uint64_t n);

/** Skip the first `n` result rows/events. */
DFTU_EXPORT dftu_view* dftu_view_offset(const dftu_view* v, uint64_t n);

/** Order collect()'s result rows by `column` (descending when `descending`
 * is nonzero). */
DFTU_EXPORT dftu_view* dftu_view_sort_by(const dftu_view* v, const char* column,
                                         int32_t descending);

/** Kind of one dftu_group_key, mirroring View::GroupKey::Kind. Arg groups on
 * an args-map entry named by `arg`; Field resolves `arg` as a field by name,
 * top-level then args (unlike Arg, it also sees top-level fields); every
 * other kind ignores `arg`. */
typedef enum {
    DFTU_GROUP_KEY_NAME = 0,
    DFTU_GROUP_KEY_CAT,
    DFTU_GROUP_KEY_PID,
    DFTU_GROUP_KEY_TID,
    DFTU_GROUP_KEY_FHASH,
    DFTU_GROUP_KEY_HHASH,
    DFTU_GROUP_KEY_IO_CAT,
    DFTU_GROUP_KEY_ACC_PAT,
    DFTU_GROUP_KEY_FILE_PATH,
    DFTU_GROUP_KEY_FILE_NAME,
    DFTU_GROUP_KEY_HOST_NAME,
    DFTU_GROUP_KEY_RANK,
    DFTU_GROUP_KEY_ARG,
    DFTU_GROUP_KEY_FIELD
} dftu_group_key_kind;

/** Value transform applied to the resolved group value before the merge key
 * is built, mirroring View::GroupKey::Transform. Only DFTU_GROUP_TRANSFORM_
 * BUCKET reads `transform_args`; every other value ignores it. */
typedef enum {
    DFTU_GROUP_TRANSFORM_NONE = 0,
    DFTU_GROUP_TRANSFORM_DIRNAME,
    DFTU_GROUP_TRANSFORM_BASENAME,
    DFTU_GROUP_TRANSFORM_LOWER,
    DFTU_GROUP_TRANSFORM_BUCKET
} dftu_group_transform;

/** One column of a (possibly composite) group-by key, mirroring
 * View::GroupKey. `transform_args` holds the Bucket substrings in priority
 * order (the first one contained in the value wins; a value matching none
 * folds to empty) and is read only when `transform` is
 * DFTU_GROUP_TRANSFORM_BUCKET. Every pointer is borrowed for the call that
 * takes this struct. */
typedef struct dftu_group_key {
    dftu_group_key_kind kind;
    const char* arg;
    dftu_group_transform transform;
    const char* const* transform_args;
    int32_t n_transform_args;
} dftu_group_key;

/** Group by the `n` composite key columns. */
DFTU_EXPORT dftu_view* dftu_view_group_by(const dftu_view* v,
                                          const dftu_group_key* keys,
                                          int32_t n);

/** Reduction ops for dftu_view_agg_spec, mirroring View::AggOp (view.h) 1:1
 * and in its declaration order, so every value is reachable and a typo is a
 * compile error rather than a silent runtime mismatch. Named dftu_view_*
 * (not dftu_agg_op) because that name is already taken: dataframe/
 * agg_op_codes.h defines a dftu_agg_op for the dataframe engine's own,
 * larger and differently-ordered aggregate vocabulary (dftu_agg_spec /
 * dftu_agg_col in the plugin ABI). The two are genuinely different
 * vocabularies - this one adds the View's occupancy ops
 * (Busy/Concurrency/Utilization/Active), which the dataframe engine has no
 * notion of - so reusing the dataframe enum would silently mismap values. */
typedef enum {
    DFTU_VIEW_AGG_COUNT = 0,
    DFTU_VIEW_AGG_SUM,
    DFTU_VIEW_AGG_MIN,
    DFTU_VIEW_AGG_MAX,
    DFTU_VIEW_AGG_MEAN,
    DFTU_VIEW_AGG_VAR,
    DFTU_VIEW_AGG_STD,
    DFTU_VIEW_AGG_ARG_MAX,
    DFTU_VIEW_AGG_SUM_SQ,
    DFTU_VIEW_AGG_PCT,
    DFTU_VIEW_AGG_SKEW,
    DFTU_VIEW_AGG_KURT,
    DFTU_VIEW_AGG_HIST,
    DFTU_VIEW_AGG_SET_UNION,
    DFTU_VIEW_AGG_BUSY,
    DFTU_VIEW_AGG_CONCURRENCY,
    DFTU_VIEW_AGG_UTILIZATION,
    DFTU_VIEW_AGG_ACTIVE
} dftu_view_agg_op;

/** One aggregate spec, mirroring View::AggSpec. `field` is the field to
 * reduce (ignored for DFTU_VIEW_AGG_COUNT; for DFTU_VIEW_AGG_ARG_MAX it is
 * the value returned, e.g. "name"). `out` names the output column. `by` is
 * read only by DFTU_VIEW_AGG_ARG_MAX: the numeric field maximized over (e.g.
 * "dur"), NULL otherwise. `q` is read only by DFTU_VIEW_AGG_PCT: the
 * quantile in (0, 1). */
typedef struct dftu_view_agg_spec {
    dftu_view_agg_op op;
    const char* field;
    const char* out;
    const char* by;
    double q;
} dftu_view_agg_spec;

/** Aggregate the `n` specs. */
DFTU_EXPORT dftu_view* dftu_view_agg(const dftu_view* v,
                                     const dftu_view_agg_spec* specs,
                                     int32_t n);

/** Run the plan and materialize the result. `rt` NULL uses the installed
 * default runtime. Caller owns the result; free with dftu_dataframe_free.
 * NULL on a scan failure. */
DFTU_EXPORT dftu_dataframe* dftu_view_collect(const dftu_view* v,
                                              dftu_runtime* rt);

/** A lazy plan whose source is this View's trace scan, so the dftu.lazy.* op
 * vocabulary (dftu_op_run_lazy) and every dftu_lazyframe_* builder run over a
 * trace scan. Builds the plan only; the scan runs on the eventual
 * dftu_lazyframe_collect. Caller owns the result; free with
 * dftu_lazyframe_free. */
DFTU_EXPORT dftu_lazyframe* dftu_view_lazy(const dftu_view* v);

#ifdef __cplusplus
}
#endif

#endif  // DFTRACER_UTILS_TRACE_VIEWS_ABI_H
