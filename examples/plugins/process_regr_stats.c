// SPDX-License-Identifier: MIT
/* Example dftracer-utils plugin in pure C: the co-moment aggregates, which
 * read `by` as x and `value` as y and stay exactly mergeable across workers.
 * Keyed by {pid}, it regresses dur on ts (slope, intercept, R2, Pearson
 * correlation) and adds the shape of dur alone (skewness, excess kurtosis).
 * The finalized dataframe is [pid, slope, intercept, r2, corr, dur_skew,
 * dur_kurt], returned from run() under "process_regr_stats".
 *
 * Build: cc -std=c99 -shared -fPIC -I<repo>/include \
 *           -o process_regr_stats.so process_regr_stats.c
 */

#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdlib.h>

static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {
    (void)self;
    *out = calloc(1, 1);
    if (!*out) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    return DFTU_OK;
}

static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                            const dftu_dataframe* df,
                            const dftu_plugin_host* host) {
    const dftu_svc_agg* agg =
        (const dftu_svc_agg*)host->get_service(host->h, DFTU_SVC_AGG);
    static const char* const keys[1] = {"pid"};
    static const dftu_agg_col specs[6] = {
        {DFTU_AGG_REGR_SLOPE, "dur", "slope", 0.0, "ts"},
        {DFTU_AGG_REGR_INTERCEPT, "dur", "intercept", 0.0, "ts"},
        {DFTU_AGG_REGR_R2, "dur", "r2", 0.0, "ts"},
        {DFTU_AGG_CORR, "dur", "corr", 0.0, "ts"},
        {DFTU_AGG_SKEW, "dur", "dur_skew", 0.0, NULL},
        {DFTU_AGG_KURT, "dur", "dur_kurt", 0.0, NULL}};
    dftu_agg* a = NULL;
    dftu_status st;
    (void)slice;
    if (!agg || !agg->agg_new)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "host lacks dftu.svc.agg");
    st =
        agg->agg_new(ctx, host->h, "process_regr_stats", keys, 1, specs, 6, &a);
    if (st < 0) return st;
    return agg->agg_accumulate(ctx, host->h, a, df);
}

static dftu_status merge(dftu_ctx* ctx, void* into, void* other) {
    (void)ctx;
    (void)into;
    (void)other;
    return DFTU_OK;
}

static dftu_status on_finalize(dftu_ctx* ctx, void* slice,
                               const dftu_plugin_host* host,
                               dftu_task** pending) {
    (void)ctx;
    (void)pending;
    (void)slice;
    (void)host;
    return DFTU_OK;
}

static void destroy_slice(void* slice) { free(slice); }

static void destroy(void* self) { (void)self; }

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("process_regr_stats", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    (void)ctx;
    (void)h;
    (void)config;
    g_plugin.struct_size = sizeof(dftu_plugin);
    g_plugin.self = NULL;
    g_plugin.plan_duql = NULL;
    g_plugin.make_slice = make_slice;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    g_plugin.on_batch = on_batch;
    *out = &g_plugin;
    return DFTU_OK;
}
