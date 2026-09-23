/* Test-only plugin that tries to register an op from on_batch instead of its
 * factory. The scan-time host must refuse: registration is a load-time
 * concern, so an op registered here would land after ordering, the prune and
 * the planner, and whether a peer saw it would depend on worker timing. The
 * plugin records the refusal so the test can assert it was refused rather
 * than silently accepted.
 */

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define OP_NAME "scan_registers_op.late"

/* Never called: the registration it belongs to is refused. Kept free of any
   engine symbol so this fixture needs no dataframe link. */
static int64_t late(const dftu_series* v) {
    (void)v;
    return 0;
}

static const dftu_op_desc g_op = {OP_NAME, DFTU_OP_SIG(I64, SERIES, NONE, NONE),
                                  (const void*)&late};

static int64_t g_refused;

static void* make_slice(void* self) {
    (void)self;
    return calloc(1, sizeof(int64_t));
}

static dftu_task* on_batch(void* slice, const dftu_dataframe* df,
                           const dftu_plugin_host* host) {
    const dftu_svc_ops* ops =
        (const dftu_svc_ops*)host->get_service(host->h, DFTU_SVC_OPS);
    (void)df;
    if (ops && ops->register_op && ops->register_op(host->h, &g_op) != 0)
        *(int64_t*)slice = 1;
    return NULL;
}

static void merge(void* into, void* other) {
    if (*(int64_t*)other) *(int64_t*)into = 1;
}

static dftu_task* on_finalize(void* slice, const dftu_plugin_host* host) {
    const dftu_svc_result* res =
        (const dftu_svc_result*)host->get_service(host->h, DFTU_SVC_RESULT);
    dftu_result_value v;
    g_refused = *(int64_t*)slice;
    if (!res) return NULL;
    memset(&v, 0, sizeof(v));
    v.kind = DFTU_RESULT_KIND_BYTES;
    v.u.bytes.data = &g_refused;
    v.u.bytes.len = sizeof(g_refused);
    res->emit(host->h, "scan_register_refused", &v);
    return NULL;
}

static void destroy_slice(void* slice) { free(slice); }
static void destroy(void* self) { (void)self; }

static dftu_plugin g_plugin;

DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(dftu_plugin_host* h,
                                                const dftu_value* config) {
    (void)h;
    (void)config;
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
