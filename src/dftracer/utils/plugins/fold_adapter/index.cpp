#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/index/extensions/plugin_extension.h>
#include <dftracer/utils/plugins/abi/index.h>
#include <dftracer/utils/plugins/fold_adapter/ext.h>
#include <dftracer/utils/plugins/reserved_names.h>

namespace dftracer::utils::plugins {
namespace {

// An extension registered mid-scan would build nothing for the files already
// indexed and prune nothing in the running query, so the scan-time host does
// not register extensions.
int host_index_register(void*, const char* name, const ::dftu_index_extension*,
                        void*) {
    DFTRACER_UTILS_LOG_ERROR(
        "Index extension '%s' refused: register from the plugin factory, "
        "which runs before the scan; the scan-time host does not register "
        "index extensions",
        name ? name : "(null)");
    return -1;
}

const ::dftu_svc_index g_index = {host_index_register};

}  // namespace

int detail::register_plugin_index_extension(const char* name,
                                            const ::dftu_index_extension* vt,
                                            void* self) {
    if (refuse_plugin_op_name(name)) {
        DFTRACER_UTILS_LOG_ERROR(
            "Index extension '%s' refused: the bare and 'dftu.' namespaces are "
            "host-reserved, so an extension name must be '<plugin>.<name>'",
            name ? name : "(null)");
        return -1;
    }
    return index::extensions::register_plugin_extension(name, vt, self) ? 0
                                                                        : -1;
}

const void* detail::index_ext_vtable() { return &g_index; }

}  // namespace dftracer::utils::plugins
