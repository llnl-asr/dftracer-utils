// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/domains/plugins/fold_adapter/ext.h>
#include <dftracer/utils/domains/plugins/reserved_names.h>
#include <dftracer/utils/dstd/fmt.h>

namespace dftracer::utils::domains::plugins {
namespace {

// As with providers, a node registered mid-scan reaches no plan built before
// it existed, so the scan-time host does not register nodes.
dftu_status host_nodes_register(::dftu_ctx* ctx, void*, const char* name,
                                const ::dftu_node_vt*, void*) {
    return host_fail(
        ctx, DFTU_E_UNSUPPORTED,
        dstd::fmt<"node '{}' refused: register from the plugin factory, which "
                  "runs before the scan; the scan-time host does not register "
                  "nodes">(dstd::StringView(name ? name : "(null)")));
}

const ::dftu_svc_nodes g_nodes = {sizeof(::dftu_svc_nodes),
                                  host_nodes_register};

}  // namespace

dftu_status detail::register_plugin_node(::dftu_ctx* ctx, const char* name,
                                         const ::dftu_node_vt* vt, void* self) {
    if (refuse_plugin_op_name(name)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"node '{}' refused: the bare and 'dftu.' namespaces are "
                      "host-reserved, so a node name must be "
                      "'<plugin>.<name>'">(
                dstd::StringView(name ? name : "(null)")));
    }
    ::dftu_error error = DFTU_ERROR_INIT;
    const dftu_status status = ::dftu_node_register(name, vt, self, &error);
    if (status < 0) {
        host_fail(ctx, status,
                  dstd::fmt<"node '{}' refused: {}">(
                      dstd::StringView(name),
                      dstd::StringView(error.message ? error.message
                                                     : "not registered")));
    }
    ::dftu_error_release(&error);
    return status;
}

const void* detail::nodes_ext_vtable() { return &g_nodes; }

}  // namespace dftracer::utils::domains::plugins
