// SPDX-License-Identifier: MIT
/* Test-only plugin whose factory registers a UDF named like a built-in duql
 * function, which the host must refuse so the load fails with a message. */

#include <dftracer/utils/plugins/abi.h>
#include <string.h>

static dftu_status bind_any(dftu_ctx* ctx, void* self,
                            const dftu_type_info* args, uint32_t n,
                            const dftu_scalar* const* consts,
                            dftu_type_info* out, void** bound) {
    (void)ctx;
    (void)self;
    (void)args;
    (void)n;
    (void)consts;
    (void)bound;
    out->struct_size = sizeof(*out);
    out->id = DFTU_TYPE_INT64;
    return DFTU_OK;
}

static dftu_status exec_none(dftu_ctx* ctx, void* bound,
                             const dftu_series* const* args, uint32_t n,
                             uint64_t rows, dftu_series** out) {
    (void)bound;
    (void)args;
    (void)n;
    (void)rows;
    (void)out;
    return dftu_fail(ctx, DFTU_E_INTERNAL, "never runs");
}

static const dftu_udf_desc CLASH = {
    sizeof(dftu_udf_desc), "abs", 0, 1, 1, bind_any, exec_none, NULL};

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("udf_clash_plugin", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    const dftu_svc_ops* ops;
    dftu_status st;
    (void)config;
    ops = (const dftu_svc_ops*)h->get_service(h->h, DFTU_SVC_OPS);
    if (!ops || !ops->register_udf)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no udf registry at load");
    st = ops->register_udf(ctx, h->h, &CLASH, NULL);
    if (st < 0) return st;
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    *out = &g_plugin;
    return DFTU_OK;
}
