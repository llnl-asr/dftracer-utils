// SPDX-License-Identifier: MIT
/* Test-only plugin that stamps a deliberately wrong ABI version in its info
 * record, so test_plugin_abi_gate.cpp can exercise the host's version gate
 * through a real dlopen instead of the build_injected_plugins() seam, which
 * skips it.
 */

#include <dftracer/utils/plugins/abi.h>

DFTU_PLUGIN_EXPORT const dftu_plugin_info* dftracer_plugin_info(void) {
    static const dftu_plugin_info info = {
        sizeof(dftu_plugin_info),
        DFTRACER_UTILS_PLUGIN_ABI_VERSION ^ 0xFFFFFFFFu, "bad_abi_version",
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
