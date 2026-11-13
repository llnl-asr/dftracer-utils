// SPDX-License-Identifier: MIT
#include <dftracer/utils/domains/plugins/fold_adapter/ext.h>
#include <dftracer/utils/domains/plugins/reserved_names.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/plugins/abi/index.h>
#include <dftracer/utils/trace/index/extensions/plugin_extension.h>

namespace dftracer::utils::domains::plugins {
namespace {

// An extension registered mid-scan would build nothing for the files already
// indexed and prune nothing in the running query, so the scan-time host does
// not register extensions.
dftu_status host_index_register(::dftu_ctx* ctx, void*, const char* name,
                                const ::dftu_index_extension*, void*) {
    return host_fail(
        ctx, DFTU_E_UNSUPPORTED,
        dstd::fmt<"index extension '{}' refused: register from the plugin "
                  "factory, which runs before the scan; the scan-time host "
                  "does not register index extensions">(
            dstd::StringView(name ? name : "(null)")));
}

const ::dftu_svc_index g_index = {sizeof(::dftu_svc_index),
                                  host_index_register};

}  // namespace

dftu_status detail::register_plugin_index_extension(
    ::dftu_ctx* ctx, const char* name, const ::dftu_index_extension* vt,
    void* self) {
    if (refuse_plugin_op_name(name)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"index extension '{}' refused: the bare and 'dftu.' "
                      "namespaces are host-reserved, so an extension name "
                      "must be '<plugin>.<name>'">(
                dstd::StringView(name ? name : "(null)")));
    }
    auto registered =
        trace::index::extensions::register_plugin_extension(name, vt, self);
    if (!registered) return host_fail(ctx, registered.error());
    return DFTU_OK;
}

const void* detail::index_ext_vtable() { return &g_index; }

}  // namespace dftracer::utils::domains::plugins
