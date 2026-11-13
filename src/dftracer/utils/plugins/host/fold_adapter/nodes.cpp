// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/node_adapter.h>
#include <dftracer/utils/plugins/host/reserved_names.h>

namespace dftracer::utils::plugins::host {
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
                                         const ::dftu_node_vt* vt, void* self,
                                         dstd::SharedPtr<PluginLibrary> library,
                                         dstd::SharedPtr<PluginMemory> memory) {
    if (refuse_plugin_op_name(name)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"node '{}' refused: the bare and 'dftu.' namespaces are "
                      "host-reserved, so a node name must be "
                      "'<plugin>.<name>'">(
                dstd::StringView(name ? name : "(null)")));
    }
    auto registered = register_node_vtable(name, vt, self, std::move(library),
                                           std::move(memory));
    if (!registered) {
        return host_fail(
            ctx, common::to_status(registered.error().condition()),
            dstd::fmt<"node '{}' refused: {}">(dstd::StringView(name),
                                               registered.error().message()));
    }
    return DFTU_OK;
}

const void* detail::nodes_ext_vtable() { return &g_nodes; }

}  // namespace dftracer::utils::plugins::host
