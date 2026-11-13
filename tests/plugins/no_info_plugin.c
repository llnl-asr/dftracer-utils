// SPDX-License-Identifier: MIT
/* Test-only plugin that exports the factory but no dftracer_plugin_info, as a
 * plugin built against the old ABI does. The loader must refuse it before it
 * runs any plugin code.
 */

#include <dftracer/utils/plugins/abi.h>

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    (void)h;
    (void)config;
    (void)out;
    return dftu_fail(ctx, DFTU_E_INTERNAL, "the loader must not get here");
}
