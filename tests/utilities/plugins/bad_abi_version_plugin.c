/* Test-only plugin that stamps a deliberately wrong ABI version, so
 * test_plugin_abi_gate.cpp can exercise the host's version gate through a real
 * dlopen instead of the build_injected_plugins() seam, which skips it.
 */

#include <dftracer/utils/plugins/abi.h>
#include <string.h>

static dftu_plugin g_plugin;

dftu_plugin* dftracer_plugin(dftu_plugin_host* h, const dftu_value* config) {
    (void)h;
    (void)config;
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.abi_version = DFTRACER_PLUGIN_ABI_VERSION ^ 0xFFFFFFFFu;
    return &g_plugin;
}
