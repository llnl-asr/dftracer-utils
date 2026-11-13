// SPDX-License-Identifier: MIT
/* Test-only plugin with the right ABI version but a header hash that differs
 * from the host's, as a plugin built against other plugin ABI headers has.
 */

#include <dftracer/utils/plugins/abi.h>

DFTU_PLUGIN_EXPORT const dftu_plugin_info* dftracer_plugin_info(void) {
    static const dftu_plugin_info info = {
        sizeof(dftu_plugin_info), DFTRACER_UTILS_PLUGIN_ABI_VERSION,
        DFTRACER_UTILS_PLUGIN_ABI_HASH ^ 0xFFFFFFFFFFFFFFFFull, "bad_abi_hash",
        "1.0"};
    return &info;
}

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    (void)h;
    (void)config;
    (void)out;
    return dftu_fail(ctx, DFTU_E_INTERNAL, "the loader must not get here");
}
