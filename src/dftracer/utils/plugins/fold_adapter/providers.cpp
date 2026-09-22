#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/fold_adapter/ext.h>
#include <dftracer/utils/plugins/reserved_names.h>

namespace dftracer::utils::plugins {
namespace {

// Registration is a load-time concern, the same reasoning host_ops_register
// applies to dftu_svc_ops: a provider registered mid-scan reaches no plan that
// was built before it existed, so the scan-time host does not register
// providers.
int host_providers_register(void*, const char* name, const ::dftu_source_vt*,
                            void*) {
    DFTRACER_UTILS_LOG_ERROR(
        "Plugin provider '%s' refused: register from the plugin factory, "
        "which runs before the scan; the scan-time host does not register "
        "providers",
        name ? name : "(null)");
    return -1;
}

const ::dftu_svc_providers g_providers = {host_providers_register};

}  // namespace

int detail::register_plugin_provider(const char* name,
                                     const ::dftu_source_vt* vt, void* self) {
    if (refuse_plugin_op_name(name)) {
        DFTRACER_UTILS_LOG_ERROR(
            "Plugin provider '%s' refused: the bare and 'dftu.' namespaces "
            "are host-reserved, so a provider name must be "
            "'<plugin>.<name>'",
            name ? name : "(null)");
        return -1;
    }
    if (::dftu_provider_register(name, vt, self) != 0) {
        DFTRACER_UTILS_LOG_ERROR(
            "Plugin provider '%s' refused: that name is already registered",
            name);
        return -1;
    }
    return 0;
}

const void* detail::providers_ext_vtable() { return &g_providers; }

}  // namespace dftracer::utils::plugins
