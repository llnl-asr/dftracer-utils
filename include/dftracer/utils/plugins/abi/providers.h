// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_ABI_PROVIDERS_H
#define DFTRACER_UTILS_PLUGINS_ABI_PROVIDERS_H

/** @file
 * dftu.svc.providers: lets a plugin register itself as a named source, so it
 * can back a @ref dftracer::utils::dataframe::LazyFrame "LazyFrame" the way a
 * file or another engine does. Registration is a build-phase concern: call
 * register_provider from the plugin factory, the same phase
 * dftu_svc_ops::register_op and dftu_svc_agg::register_state run in. Include
 * dftracer/utils/plugins/abi.h rather than this file directly.
 *
 * A registered dftu_source_vt (defined below) is a C mirror
 * of the C++ dftracer::utils::dataframe::Source / Cursor pair (lazyframe.h):
 * schema() without a scan, scan() opens a Cursor honoring a dftu_scan_request
 * (projection/filters/limit pushdown), and the Cursor pulls morsels one
 * dftu_dataframe at a time. A provider that ignores the request and reports
 * DFTU_PUSHED_NO for every filter is always sound, since the host re-applies
 * whatever a source did not translate; only projection is not advisory - a
 * provider accepting a non-empty projection must return exactly those
 * columns, in that order, or the host surfaces a scan error.
 *
 * register_provider wraps the vtable, after the plugin-name gate, as a C++
 * provider in the process-global registry that dftu_lazyframe_from_provider
 * (dftracer/utils/dataframe/abi.h) reads. The registry has no C entry to
 * register: only the host builds the adapter from a vtable, so the C ABI of
 * dataframe stays free of plugin types.
 */

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi/core.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DFTU_SVC_PROVIDERS "dftu.svc.providers"

/** A pull cursor over one open scan of a registered source. Exactly one
   thread drives a given cursor at a time; the host never calls next() again
   before a prior call's task (if any) has completed. Every callback that can
   fail takes the host's @ref dftu_ctx first and returns a @ref dftu_status; a
   negative status is an error with the ctx message, never the end of the
   stream. */
typedef struct dftu_cursor_vt {
    uint32_t struct_size; /**< sizeof(dftu_cursor_vt) */
    /** Pull up to max_rows rows. DFTU_OK with `*out` a NEW dftu_dataframe the
       host frees with dftu_dataframe_free, DFTU_END (and `*out` NULL) at the
       end of the stream, or an error. With `*pending` set to a task the
       host awaits (see dftu_task), the host reads `*out` and the task's status
       only after it completes; the task follows the ownership rules of
       dftu_task. */
    dftu_status (*next)(dftu_ctx* ctx, void* self, int64_t max_rows,
                        dftu_dataframe** out, dftu_task** pending);
    /** Release a cursor from source_vt::scan; exactly one call per scan(). */
    void (*destroy)(void* self);
    /** Optional (may be NULL). Offer `predicate` (positional against this
       cursor's own columns, borrowed for the call) as a narrowing of the
       rows it has not produced yet: a join's build side, or a node that
       learns a bound partway through, sends the key set or range it will
       keep. ADVISORY: skip only rows `predicate` is false for, or ignore it;
       every row still produced is checked again downstream, so honouring it
       changes only the work done, never the result. A node that holds an
       input cursor forwards it to in_vt->narrow when the predicate is
       positional against its input as well (a node that only adds or keeps
       columns), remapping or refusing otherwise. Same pending convention as
       next(): the host awaits `*pending` before reading *out_applied
       (nonzero when the cursor did anything with it, for diagnostics). */
    dftu_status (*narrow)(dftu_ctx* ctx, void* self, const dftu_expr* predicate,
                          int32_t* out_applied, dftu_task** pending);
    /** Optional (may be NULL). Bytes this cursor holds resident beyond one
       in-flight frame: a build table, a spool, a cache. The host sums it
       over the plan against the memory budget; without it the cursor is
       never asked to reclaim. */
    uint64_t (*bytes)(void* self);
    /** Optional (may be NULL). Free up to `want` resident bytes (spill to
       disk, compact, drop a cache) and return how many were freed; less, or
       0, is allowed. The rows the cursor produces must not change. Called
       when the plan runs past its budget, largest holder first. */
    uint64_t (*reclaim)(void* self, uint64_t want);
} dftu_cursor_vt;

/** How completely a source applied one pushed-down filter (mirrors
   dataframe::Pushed, lazyframe.h). Passed back through scan()'s out_pushed
   array, one entry per dftu_scan_request::filters. The host pre-fills every
   entry with DFTU_PUSHED_NO before calling scan() and treats a value outside
   this set as DFTU_PUSHED_NO rather than trusting it - a source has no way to
   prove an Exact claim, so a wrong one silently drops rows and must never be
   taken on faith. */
typedef int32_t dftu_pushed;
enum {
    DFTU_PUSHED_NO = 0,      /**< Not applied; the host applies it. */
    DFTU_PUSHED_INEXACT = 1, /**< Pruned I/O but did not filter survivors. */
    DFTU_PUSHED_EXACT = 2,   /**< Fully applied; the host drops it. */
};

/** A pushdown request passed to dftu_source_vt::scan (mirrors
   @ref dftracer::utils::dataframe::ScanRequest "ScanRequest", lazyframe.h). */
typedef struct dftu_scan_request {
    /** Columns the plan needs, in the order the scan must return them. NULL
       or n_projection == 0 means all source columns. Accepting a non-empty
       projection means every returned dftu_dataframe MUST carry exactly
       these columns, in this order. */
    const char* const* projection;
    int32_t n_projection;
    /** Candidate predicates, borrowed for the call, each positional against
       the request's column set (projection when non-empty, else schema()).
       Translate what can be pushed and report the rest DFTU_PUSHED_NO. */
    const dftu_expr* const* filters;
    int32_t n_filters;
    int64_t limit;          /**< Slice pushdown hint; -1 means none. */
    uint64_t memory_budget; /**< Bytes; most sources ignore it. */
} dftu_scan_request;

/** Which op a dftu_apply_request offers (mirrors the Source::apply_* hooks,
   lazyframe.h). */
typedef int32_t dftu_apply_kind;
enum {
    DFTU_APPLY_FILTER = 0,
    DFTU_APPLY_PROJECTION = 1,
    DFTU_APPLY_AGGREGATION = 2,
    DFTU_APPLY_SORT = 3,
    DFTU_APPLY_TOPN = 4,
    DFTU_APPLY_LIMIT = 5,
    DFTU_APPLY_TAIL = 6,
    DFTU_APPLY_JOIN = 7,
};

/** How a source took an offered op (mirrors dataframe::ApplyStatus). The host
   pre-fills DFTU_APPLY_NO_CHANGE and treats any value outside this set as
   DFTU_APPLY_NO_CHANGE. */
typedef int32_t dftu_apply_status;
enum {
    DFTU_APPLY_NO_CHANGE = 0, /**< Unsupported or no change; no new source. */
    DFTU_APPLY_EXACT = 1,     /**< Fully applied; the host drops the op. */
    DFTU_APPLY_INEXACT = 2,   /**< Rows narrowed only; the host keeps the op. */
};

/** One projection output column: `expr` is positional against the offering
   source's schema(). */
typedef struct dftu_named_expr {
    const char* name;
    const dftu_expr* expr;
} dftu_named_expr;

/** One aggregate of an offered group-by (mirrors @ref
   dftracer::utils::dataframe::AggregateExpr "AggregateExpr"). `op` is the
   canonical aggregate name (dataframe::to_string(Agg)). `input` and `by` are
   positional against schema(); `input` is NULL for count and `by` is NULL
   unless the op reads a second column. `param` is the quantile or k where the
   op takes one. */
typedef struct dftu_apply_agg {
    const char* op;
    const dftu_expr* input;
    const dftu_expr* by;
    double param;
    const char* out;
} dftu_apply_agg;

/** FILTER: keep the rows where `predicate`, positional against schema(),
   is true. */
typedef struct dftu_apply_filter_args {
    const dftu_expr* predicate;
} dftu_apply_filter_args;

/** PROJECTION: the output is exactly `exprs[n_exprs]`, in order. */
typedef struct dftu_apply_projection_args {
    const dftu_named_expr* exprs;
    int32_t n_exprs;
} dftu_apply_projection_args;

/** AGGREGATION: group by `keys[n_keys]` and compute `aggs[n_aggs]`. The
   output is one column per key, named by its `name`, in order, then one
   column per agg named by its `out`. */
typedef struct dftu_apply_aggregation_args {
    const dftu_named_expr* keys;
    int32_t n_keys;
    const dftu_apply_agg* aggs;
    int32_t n_aggs;
} dftu_apply_aggregation_args;

/** SORT: order by `by[n_by]`, `descending[i]` nonzero for descending. */
typedef struct dftu_apply_sort_args {
    const char* const* by;
    const int32_t* descending;
    int32_t n_by;
} dftu_apply_sort_args;

/** TOPN: the first `k` rows of SORT by `sort`. */
typedef struct dftu_apply_topn_args {
    dftu_apply_sort_args sort;
    int64_t k;
} dftu_apply_topn_args;

/** LIMIT: rows [`offset`, `offset + n`) in source order. */
typedef struct dftu_apply_limit_args {
    int64_t offset;
    int64_t n;
} dftu_apply_limit_args;

/** TAIL: the last `n` rows in source order. */
typedef struct dftu_apply_tail_args {
    int64_t n;
} dftu_apply_tail_args;

/** JOIN: this source joined with `other_self`/`other_vt`, a source of the same
   provider with nothing left to run above it, on `left_on[n_on]` =
   `right_on[n_on]` by `how` (dftu_join_how), suffixing clashing right columns
   with `suffix`. The output is what the engine's join makes for the same
   arguments. `other_self` is borrowed for the call; the host keeps the other
   source alive for as long as the derived source. `other_vt` is the host's
   copy of that source's vtable, so recognize your own source by one of its
   callbacks (for example `other_vt->apply`), not by the vtable address. */
typedef struct dftu_apply_join_args {
    void* other_self;
    const struct dftu_source_vt* other_vt;
    const char* const* left_on;
    const char* const* right_on;
    int32_t n_on;
    int32_t how; /**< dftu_join_how */
    const char* suffix;
} dftu_apply_join_args;

/** An op offered to dftu_source_vt::apply: `kind` selects the one member of
   `u` that is set. Every pointer is borrowed for the call. */
typedef struct dftu_apply_request {
    int32_t kind; /**< dftu_apply_kind */
    union {
        dftu_apply_filter_args filter;
        dftu_apply_projection_args projection;
        dftu_apply_aggregation_args aggregation;
        dftu_apply_sort_args sort;
        dftu_apply_topn_args topn;
        dftu_apply_limit_args limit;
        dftu_apply_tail_args tail;
        dftu_apply_join_args join;
    } u;
} dftu_apply_request;

/** The answer to dftu_source_vt::apply. For DFTU_APPLY_EXACT or
   DFTU_APPLY_INEXACT, `self` and `vt` are a NEW derived source the host owns:
   it calls vt->destroy(self) exactly once, after every cursor it opened and
   every source derived from it are gone. `vt` must outlive that source.
   DFTU_APPLY_NO_CHANGE returns no source. */
typedef struct dftu_apply_result {
    int32_t status; /**< dftu_apply_status */
    void* self;
    const struct dftu_source_vt* vt;
} dftu_apply_result;

/** A pushdown-aware data source a plugin registers under a name. Immutable:
   schema() reports columns without scanning and scan() may be called many
   times to open independent cursors, so one registered source can back many
   @ref dftracer::utils::dataframe::LazyFrame "LazyFrame" collects. */
typedef struct dftu_source_vt {
    uint32_t struct_size; /**< sizeof(dftu_source_vt) */
    /** Column names, as a NUL-terminated array owned by the source and valid
       for its lifetime (until destroy()); `*out_count` is their number. */
    dftu_status (*schema)(dftu_ctx* ctx, void* self,
                          const char* const** out_names, int32_t* out_count);
    /** Open a cursor honoring `req`. `out_pushed` has req->n_filters entries,
       pre-filled DFTU_PUSHED_NO by the host; write one dftu_pushed per
       req->filters entry, leaving an untranslated filter DFTU_PUSHED_NO.
       On success, *out_cursor_self receives the cursor's `self` (it may be
       NULL for a stateless cursor) and *out_vt its vtable, which must outlive
       the cursor.
       When req->projection is non-empty, every dftu_dataframe the cursor
       produces (sync or via the awaited task) MUST carry exactly those
       columns, in that order; the host verifies this and surfaces a scan
       error rather than passing a mismatched frame downstream. */
    dftu_status (*scan)(dftu_ctx* ctx, void* self, const dftu_scan_request* req,
                        int32_t* out_pushed, void** out_cursor_self,
                        const dftu_cursor_vt** out_vt);
    /** Release the source; called once, after every cursor it opened has
       been destroyed. */
    void (*destroy)(void* self);
    /** Optional (NULL is fine): declare column types by appending exactly
       schema()'s columns, in the same order and with the same names, to
       `out_schema` - an empty dftu_schema the host allocates and frees
       around this one call. A count or name mismatch is discarded: every
       column reports Unknown, the same as if this were NULL. */
    dftu_status (*schema_types)(dftu_ctx* ctx, void* self,
                                dftu_schema* out_schema);
    /** Optional (NULL means no planning hooks): absorb the op `req` offers at
       plan time, before scan(). `out` is pre-filled DFTU_APPLY_NO_CHANGE.
       Accepting yields a new derived source (see dftu_apply_result); offering
       the same op to that derived source again must answer
       DFTU_APPLY_NO_CHANGE. PROJECTION, AGGREGATION and JOIN change the
       schema and may only be DFTU_APPLY_EXACT; the host refuses an INEXACT
       answer to them. The derived source's schema/schema_types report its
       output. */
    dftu_status (*apply)(dftu_ctx* ctx, void* self,
                         const dftu_apply_request* req, dftu_apply_result* out);
} dftu_source_vt;

/** Build-phase registry of named sources. Fetched via
   dftu_plugin_host::get_service(DFTU_SVC_PROVIDERS); the run-time host
   (inside a fold callback) still answers this group so a plugin sees a
   consistent service surface, but refuses register_provider there - by the
   time a scan runs, the set of providers a plan can name is already
   settled. */
typedef struct dftu_svc_providers {
    uint32_t struct_size; /**< sizeof(dftu_svc_providers) */
    /** Register `vt`/`self` under `name`. `name` must be `<plugin>.<name>`
       (the bare and "dftu." namespaces are host-reserved, the same rule
       dftu_svc_ops::register_op enforces); `vt` is copied, so it need not
       outlive the call, but `self` must outlive every scan the host later
       opens against it. Fails with DFTU_E_INVALID if `name` is NULL or
       empty, `vt` is NULL, `name` fails the namespace rule, or the name is
       already registered - a second registration under the same name is
       refused, never a silent replace. */
    dftu_status (*register_provider)(dftu_ctx* ctx, void* h, const char* name,
                                     const dftu_source_vt* vt, void* self);
} dftu_svc_providers;

#ifdef __cplusplus
}
#endif

#endif /* DFTRACER_UTILS_PLUGINS_ABI_PROVIDERS_H */
