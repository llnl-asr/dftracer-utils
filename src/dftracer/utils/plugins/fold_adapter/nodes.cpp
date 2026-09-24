#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/fold_adapter/ext.h>
#include <dftracer/utils/plugins/reserved_names.h>

namespace dftracer::utils::plugins {
namespace {

// As with providers, a node registered mid-scan reaches no plan built before
// it existed, so the scan-time host does not register nodes.
int host_nodes_register(void*, const char* name, const ::dftu_node_vt*, void*) {
    DFTRACER_UTILS_LOG_ERROR(
        "Plugin node '%s' refused: register from the plugin factory, which "
        "runs before the scan; the scan-time host does not register nodes",
        name ? name : "(null)");
    return -1;
}

const ::dftu_svc_nodes g_nodes = {host_nodes_register};

}  // namespace

int detail::register_plugin_node(const char* name, const ::dftu_node_vt* vt,
                                 void* self) {
    if (refuse_plugin_op_name(name)) {
        DFTRACER_UTILS_LOG_ERROR(
            "Plugin node '%s' refused: the bare and 'dftu.' namespaces are "
            "host-reserved, so a node name must be '<plugin>.<name>'",
            name ? name : "(null)");
        return -1;
    }
    if (::dftu_node_register(name, vt, self) != 0) {
        DFTRACER_UTILS_LOG_ERROR(
            "Plugin node '%s' refused: the name is already registered or the "
            "vtable lacks output_schema/open",
            name);
        return -1;
    }
    return 0;
}

const void* detail::nodes_ext_vtable() { return &g_nodes; }

}  // namespace dftracer::utils::plugins
