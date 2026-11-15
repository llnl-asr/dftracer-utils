// SPDX-License-Identifier: MIT
/* Test-only plugin whose dftu_plugin struct_size is below the first published
 * size, so the host refuses it after the factory returns the descriptor.
 */

#include <dftracer/utils/plugins/abi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void destroy_should_not_run(void* self) {
    (void)self;
    abort();
}

static dftu_plugin g_plugin;

DFTU_PLUGIN_INFO("too_small_plugin", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    (void)ctx;
    (void)h;
    (void)config;
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.struct_size = (uint32_t)sizeof(uint32_t);
    /* Past the declared size: a rejected descriptor must never run destroy. */
    g_plugin.destroy = destroy_should_not_run;
    *out = &g_plugin;
    return DFTU_OK;
}
