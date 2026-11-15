// SPDX-License-Identifier: MIT
/* Test-only plugin whose dftu_plugin_info stops before the optional version
 * field. The host copies the declared prefix into a zeroed record, so version
 * reads NULL and the plugin still loads.
 */

#include <dftracer/utils/plugins/abi.h>
#include <stddef.h>
#include <string.h>

static void destroy(void* self) { (void)self; }

static dftu_plugin g_plugin;

DFTU_PLUGIN_EXPORT const dftu_plugin_info* dftracer_plugin_info(void) {
    static const dftu_plugin_info info = {
        (uint32_t)offsetof(dftu_plugin_info, version),
        DFTRACER_UTILS_PLUGIN_ABI_VERSION, "short_info_plugin", NULL};
    return &info;
}

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    (void)ctx;
    (void)h;
    (void)config;
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = sizeof(g_plugin);
    g_plugin.destroy = destroy;
    *out = &g_plugin;
    return DFTU_OK;
}
