// SPDX-License-Identifier: MIT
/* Example dftracer-utils plugin in pure C: the set-valued and list-valued
 * aggregates, all in one accumulator keyed by {pid}. SET_UNION gives the
 * distinct event names a process ran, DISTINCT their approximate cardinality
 * (a KMV sketch, `param` = k) and LIST_SORTED the whole name sequence ordered
 * by the `by` column ts. The host merges the accumulator across workers and
 * finalizes it to [pid, names, n_names, name_seq], where names is a joined
 * string and name_seq a list<string> column, returned from run() under
 * "process_collections".
 *
 * Build: cc -std=c99 -shared -fPIC -I<repo>/include \
 *           -o process_collections.so process_collections.c
 */

#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdlib.h>

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
    static const char* const keys[1] = {"pid"};
    static const dftu_agg_col specs[3] = {
        {DFTU_AGG_SET_UNION, "name", "names", 0.0, NULL},
        {DFTU_AGG_DISTINCT, "name", "n_names", 1024.0, NULL},
        {DFTU_AGG_LIST_SORTED, "name", "name_seq", 0.0, "ts"}};
    dftu_agg* a = NULL;
    dftu_status st;
    (void)slice;
    if (!agg || !agg->agg_new)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "host lacks dftu.svc.agg");
    st = agg->agg_new(ctx, host->h, "process_collections", keys, 1, specs, 3,
                      &a);
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

DFTU_PLUGIN_INFO("process_collections", "1.0")

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
