// SPDX-License-Identifier: MIT
/* Test-only plugin whose dftu_plugin record stops before the last optional
 * callback, transform. The host copies only the declared prefix into a zeroed
 * record, so transform stays NULL and the fold still runs through on_batch.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/plugins/abi.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int64_t g_total;

static dftu_status make_slice(dftu_ctx* ctx, void* self, void** out) {
    (void)self;
    *out = calloc(1, sizeof(int64_t));
    if (!*out) return dftu_fail(ctx, DFTU_E_NO_MEMORY, "out of memory");
    return DFTU_OK;
}

static dftu_status on_batch(dftu_ctx* ctx, void* slice,
                            const dftu_dataframe* df,
                            const dftu_plugin_host* host) {
    (void)ctx;
    (void)host;
    *(int64_t*)slice += dftu_dataframe_num_rows(df);
    return DFTU_OK;
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
    return res->emit(ctx, host->h, "seen_rows", &v);
}

static void destroy_slice(void* slice) { free(slice); }
static void destroy(void* self) { (void)self; }

static dftu_status transform_should_not_run(dftu_ctx* ctx, void* slice,
                                            const dftu_dataframe* df,
                                            const dftu_plugin_host* host,
                                            dftu_dataframe** out) {
    (void)ctx;
    (void)slice;
    (void)df;
    (void)host;
    (void)out;
    abort();
}

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("short_plugin", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    (void)ctx;
    (void)h;
    (void)config;
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = (uint32_t)offsetof(dftu_plugin, transform);
    g_plugin.make_slice = make_slice;
    g_plugin.on_batch = on_batch;
    g_plugin.merge = merge;
    g_plugin.on_finalize = on_finalize;
    g_plugin.destroy_slice = destroy_slice;
    g_plugin.destroy = destroy;
    /* Past the declared size: the host must read only the checked prefix. */
    g_plugin.transform = transform_should_not_run;
    *out = &g_plugin;
    return DFTU_OK;
}
