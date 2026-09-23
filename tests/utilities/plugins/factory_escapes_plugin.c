/* Test-only plugin whose factory asks the build-phase host for a service the
 * load phase does not have. The host must answer NULL and refuse the load
 * naming the service, rather than handing over a half-built host.
 */

#include <dftracer/utils/plugins/abi.h>
#include <string.h>

static dftu_plugin g_plugin;

DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(dftu_plugin_host* h,
                                                const dftu_value* config) {
    const dftu_io* io;
    (void)config;
    io = (const dftu_io*)h->get_service(h->h, DFTU_SVC_IO);
    if (!io) {
        h->log(h->h, DFTU_LOG_ERROR, "no I/O service at load", 22);
        return NULL;
    }
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    return &g_plugin;
}
