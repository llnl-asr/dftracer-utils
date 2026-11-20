// SPDX-License-Identifier: MIT
/* Test-only plugin whose index extension has neither step nor step_column, so
 * the host refuses to register it and the plugin fails to load. */

#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <string.h>

static dftu_status make_builder(dftu_ctx* ctx, void* self, void** out) {
    (void)ctx;
    (void)self;
    *out = NULL;
    return DFTU_OK;
}
static dftu_status finish_granule(dftu_ctx* ctx, void* b, uint64_t granule,
                                  const dftu_index_out* out) {
    (void)ctx;
    (void)b;
    (void)granule;
    (void)out;
    return DFTU_OK;
}
static void destroy_builder(void* b) { (void)b; }
static dftu_status compile(dftu_ctx* ctx, void* self,
                           const dftu_index_leaf* leaf, void** out) {
    (void)ctx;
    (void)self;
    (void)leaf;
    *out = NULL;
    return DFTU_OK;
}
static int may_match(void* h, const uint8_t* p, uint64_t n) {
    (void)h;
    (void)p;
    (void)n;
    return 1;
}
static void release(void* h) { (void)h; }

static dftu_index_extension g_ext;
static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("index_no_step", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    const dftu_svc_index* index =
        (const dftu_svc_index*)h->get_service(h->h, DFTU_SVC_INDEX);
    dftu_status st;
    (void)config;
    if (!index)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no index service at load");
    memset(&g_ext, 0, sizeof(g_ext));
    g_ext.struct_size = sizeof(g_ext);
    g_ext.version = 1;
    g_ext.make_builder = make_builder;
    g_ext.finish_granule = finish_granule;
    g_ext.destroy_builder = destroy_builder;
    g_ext.compile = compile;
    g_ext.may_match = may_match;
    g_ext.release = release;
    st = index->register_extension(ctx, h->h, "index_no_step.none", &g_ext,
                                   NULL);
    if (st < 0) return st;
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    *out = &g_plugin;
    return DFTU_OK;
}
