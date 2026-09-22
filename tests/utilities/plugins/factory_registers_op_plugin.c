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
                                  (const void*)&double_rows};

static int64_t g_total;

static void* make_slice(void* self) {
    (void)self;
    return calloc(1, sizeof(int64_t));
}

static dftu_task* on_batch(void* slice, const dftu_dataframe* df,
                           const dftu_plugin_host* host) {
    const dftu_svc_ops* ops =
        (const dftu_svc_ops*)host->get_service(host->h, DFTU_SVC_OPS);
    dftu_series* dur = dftu_dataframe_column(df, "dur");
    dftu_result_scalar r;
    if (!ops || !dur) return NULL;
    r = ops->run_aggregate(host->h, OP_NAME, (const dftu_series* const*)&dur, 1,
                           NULL);
    if (DFTU_RESULT_OK(r)) *(int64_t*)slice += DFTU_RESULT_VALUE(r).value.i;
    dftu_series_free(dur);
    return NULL;
}

static void merge(void* into, void* other) {
    *(int64_t*)into += *(int64_t*)other;
}

static dftu_task* on_finalize(void* slice, const dftu_plugin_host* host) {
    const dftu_svc_result* res =
        (const dftu_svc_result*)host->get_service(host->h, DFTU_SVC_RESULT);
    dftu_result_value v;
    g_total = *(int64_t*)slice;
    if (!res) return NULL;
    memset(&v, 0, sizeof(v));
    v.kind = DFTU_RESULT_KIND_BYTES;
    v.u.bytes.data = &g_total;
    v.u.bytes.len = sizeof(g_total);
    res->emit(host->h, "doubled_rows", &v);
    return NULL;
}

static void destroy_slice(void* slice) { free(slice); }
static void destroy(void* self) { (void)self; }

static dftu_plugin g_plugin;

DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(dftu_plugin_host* h,
                                                const dftu_value* config) {
    const dftu_svc_ops* ops;
    (void)config;
    ops = (const dftu_svc_ops*)h->get_service(h->h, DFTU_SVC_OPS);
    if (!ops || !ops->register_op) return NULL;
    /* The op registry is process-global, so a second load of this plugin in
       the same process finds the name taken; either way it is registered. */
    if (ops->register_op(h->h, &g_op) != 0 && !ops->find(h->h, OP_NAME))
        return NULL;

    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    g_plugin.make_slice = make_slice;
    g_plugin.on_batch = on_batch;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    return &g_plugin;
}
