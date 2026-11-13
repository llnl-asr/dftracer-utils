// SPDX-License-Identifier: MIT
/* Test-only plugin whose factory registers a dataframe op at load and then
 * runs that op during the scan. Proves registration from the factory happens
 * early enough to matter: the op is in the registry before the first batch,
 * not after it.
 */

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OP_NAME "factory_registers_op.double_rows"

static int64_t double_rows(const dftu_series* v) {
    return 2 * (int64_t)dftu_series_length(v);
}

static const dftu_op_desc g_op = {OP_NAME, DFTU_OP_SIG(I64, SERIES, NONE, NONE),
                                  (dftu_op_generic_fn)&double_rows};

static int64_t g_total;

static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {
    (void)self;
    *out = calloc(1, sizeof(int64_t));
    if (!*out) return dftu_fail(ctx, DFTU_E_INTERNAL, "out of memory");
    return DFTU_OK;
}

static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                            const dftu_dataframe* df,
                            const dftu_plugin_host* host) {
    const dftu_svc_ops* ops =
        (const dftu_svc_ops*)host->get_service(host->h, DFTU_SVC_OPS);
    dftu_series* dur = dftu_dataframe_column(df, "dur");
    dftu_scalar r;
    dftu_status st;
    if (!ops || !dur) return DFTU_OK;
    st = ops->run_aggregate(ctx, host->h, OP_NAME,
                            (const dftu_series* const*)&dur, 1, NULL, &r);
    if (st >= 0) *(int64_t*)slice += r.value.i;
    dftu_series_free(dur);
    return st;
}

static dftu_status merge(dftu_ctx* ctx, void* into, void* other) {
    (void)ctx;
    *(int64_t*)into += *(int64_t*)other;
    return DFTU_OK;
}

static dftu_status on_finalize(dftu_ctx* ctx, void* slice,
                               const dftu_plugin_host* host,
                               dftu_task** pending) {
    const dftu_svc_result* res =
        (const dftu_svc_result*)host->get_service(host->h, DFTU_SVC_RESULT);
    dftu_result_value v;
    (void)pending;
    g_total = *(int64_t*)slice;
    if (!res) return DFTU_OK;
    memset(&v, 0, sizeof(v));
    v.kind = DFTU_RESULT_KIND_BYTES;
    v.u.bytes.data = &g_total;
    v.u.bytes.len = sizeof(g_total);
    return res->emit(ctx, host->h, "doubled_rows", &v);
}

static void destroy_slice(void* slice) { free(slice); }
static void destroy(void* self) { (void)self; }

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("factory_registers_op", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    const dftu_svc_ops* ops;
    (void)config;
    ops = (const dftu_svc_ops*)h->get_service(h->h, DFTU_SVC_OPS);
    if (!ops || !ops->register_op)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no op registry at load");
    /* The op registry is process-global, so a second load of this plugin in
       the same process finds the name taken; either way it is registered. */
    if (ops->register_op(ctx, h->h, &g_op) < 0 && !ops->find(h->h, OP_NAME))
        return dftu_fail(ctx, DFTU_E_INVALID, "op registration refused");

    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    g_plugin.make_slice = make_slice;
    g_plugin.on_batch = on_batch;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    *out = &g_plugin;
    return DFTU_OK;
}
