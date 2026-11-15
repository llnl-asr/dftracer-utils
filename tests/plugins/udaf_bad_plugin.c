// SPDX-License-Identifier: MIT
/* Test-only plugin whose factory registers a UDAF with a state that is not
 * plain bytes and has no serialize pair, which the host must refuse so the
 * load fails with a message. */

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

static void init_none(void* bound, void* const* states, uint64_t n) {
    (void)bound;
    (void)states;
    (void)n;
}

static dftu_status update_none(dftu_ctx* ctx, void* bound, void* const* states,
                               const dftu_series* const* args, uint32_t nargs,
                               uint64_t rows) {
    (void)ctx;
    (void)bound;
    (void)states;
    (void)args;
    (void)nargs;
    (void)rows;
    return DFTU_OK;
}

static dftu_status combine_none(dftu_ctx* ctx, void* bound, void* const* into,
                                void* const* from, uint64_t n) {
    (void)ctx;
    (void)bound;
    (void)into;
    (void)from;
    (void)n;
    return DFTU_OK;
}

static dftu_status finalize_none(dftu_ctx* ctx, void* bound,
                                 void* const* states, uint64_t n,
                                 dftu_series** out) {
    (void)bound;
    (void)states;
    (void)n;
    (void)out;
    return dftu_fail(ctx, DFTU_E_INTERNAL, "never runs");
}

static void destroy_none(void* bound, void* const* states, uint64_t n) {
    (void)bound;
    (void)states;
    (void)n;
}

static const dftu_udaf_desc BAD = {sizeof(dftu_udaf_desc),
                                   "ubad",
                                   0,
                                   1,
                                   1,
                                   8,
                                   8,
                                   bind_any,
                                   init_none,
                                   update_none,
                                   combine_none,
                                   finalize_none,
                                   destroy_none,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL};

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("udaf_bad_plugin", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    const dftu_svc_ops* ops;
    dftu_status st;
    (void)config;
    ops = (const dftu_svc_ops*)h->get_service(h->h, DFTU_SVC_OPS);
    if (!ops || !ops->register_udaf)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no udaf registry at load");
    st = ops->register_udaf(ctx, h->h, &BAD, NULL);
    if (st < 0) return st;
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    *out = &g_plugin;
    return DFTU_OK;
}
