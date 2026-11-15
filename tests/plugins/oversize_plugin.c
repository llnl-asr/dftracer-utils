// SPDX-License-Identifier: MIT
/* Test-only plugin whose dftu_plugin struct_size is larger than the host's,
 * so the host refuses the descriptor instead of copying past the record.
 */

#include <dftracer/utils/plugins/abi.h>
#include <string.h>

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("oversize_plugin", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    (void)ctx;
    (void)h;
    (void)config;
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = (uint32_t)sizeof(g_plugin) + 1;
    *out = &g_plugin;
    return DFTU_OK;
}
