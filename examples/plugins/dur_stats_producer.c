// SPDX-License-Identifier: MIT
/* Example producer plugin: a ZERO-KEY dft.ext.agg accumulator, the AggState
 * form of a scalar handle. Each batch arrives as a column dataframe and is
 * folded whole into one accumulator named com.example.dur_stats, computing the
 * whole-scan event count and the duration median, min and max. A consumer
 * reads it back by that name. The host merges the accumulator across
 * every worker slice, so a consumer reads stats no single slice ever saw.
 *
 * Build: cc -std=c99 -shared -fPIC -I<repo>/include \
 *           -o dur_stats_producer.so dur_stats_producer.c
 */

#include <dftracer/utils/plugins/abi.h>
#include <stdlib.h>
#include <string.h>

#define DUR_STATS_PORT "com.example.dur_stats"

static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {
    (void)self;
    *out = calloc(1, 1);
    if (!*out) return dftu_fail(ctx, DFTU_E_INTERNAL, "out of memory");
    return DFTU_OK;
}

static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                            const dftu_dataframe* df,
                            const dftu_plugin_host* host) {
    const dftu_svc_agg* agg =
        (const dftu_svc_agg*)host->get_service(host->h, DFTU_SVC_AGG);
    static const dftu_agg_col specs[4] = {
        {DFTU_AGG_COUNT, NULL, "count", 0.0, NULL},
        {DFTU_AGG_PCT, "dur", "p50", 0.5, NULL},
        {DFTU_AGG_MIN, "dur", "min", 0.0, NULL},
        {DFTU_AGG_MAX, "dur", "max", 0.0, NULL}};
    dftu_agg* a = NULL;
    dftu_status st;
    (void)slice;
    if (!agg || !agg->agg_new)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "host lacks dftu.svc.agg");
    /* No key columns: the whole scan is one group, i.e. a scalar handle. */
    st = agg->agg_new(ctx, host->h, DUR_STATS_PORT, NULL, 0, specs, 4, &a);
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

static const char* const provided[2] = {DUR_STATS_PORT, NULL};

static const char* const* provides(void* self) {
    (void)self;
    return provided;
}

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("dur_stats_producer", "1.0")

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
    g_plugin.provides = provides;
    *out = &g_plugin;
    return DFTU_OK;
}
