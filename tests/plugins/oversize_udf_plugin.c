// SPDX-License-Identifier: MIT
/* Test-only plugin whose factory registers a UDF with a struct_size larger
 * than the host's dftu_udf_desc, so the registration is refused.
 */

#include <dftracer/utils/plugins/abi.h>
#include <string.h>

DFTU_PLUGIN_INFO("oversize_udf_plugin", "1.0")

DFTU_PLUGIN_EXPORT dftu_status dftracer_plugin(dftu_ctx* ctx,
                                               dftu_plugin_host* h,
                                               const dftu_value* config,
                                               dftu_plugin** out) {
    const dftu_svc_ops* ops =
        (const dftu_svc_ops*)h->get_service(h->h, DFTU_SVC_OPS);
    dftu_udf_desc desc;
    dftu_status st;
    (void)config;
    (void)out;
    if (!ops || !ops->register_udf)
        return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "no udf registry at load");
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = (uint32_t)sizeof(desc) + 1;
    desc.name = "oversize_udf";
    st = ops->register_udf(ctx, h->h, &desc, NULL);
    if (st < 0) return st;
    return dftu_fail(ctx, DFTU_E_INTERNAL, "the loader must not get here");
}
