// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/provider_source.h>
#include <dftracer/utils/plugins/host/reserved_names.h>

namespace dftracer::utils::plugins::host {
namespace {

// Registration is a load-time concern, the same reasoning host_ops_register
// applies to dftu_svc_ops: a provider registered mid-scan reaches no plan that
// was built before it existed, so the scan-time host does not register
// providers.
dftu_status host_providers_register(::dftu_ctx* ctx, void*, const char* name,
                                    const ::dftu_source_vt*, void*) {
    return host_fail(
        ctx, DFTU_E_UNSUPPORTED,
        dstd::fmt<"provider '{}' refused: register from the plugin factory, "
                  "which runs before the scan; the scan-time host does not "
                  "register providers">(
            dstd::StringView(name ? name : "(null)")));
}

const ::dftu_svc_providers g_providers = {sizeof(::dftu_svc_providers),
                                          host_providers_register};

}  // namespace

dftu_status detail::register_plugin_provider(
    ::dftu_ctx* ctx, dstd::StringView plugin, const char* name,
    const ::dftu_source_vt* vt, void* self,
    dstd::SharedPtr<PluginLibrary> library,
    dstd::SharedPtr<PluginMemory> memory) {
    if (!name_in_plugin_namespace(name, plugin)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"provider '{}' refused: a provider name must be '{}' or "
                      "'{}.<name>'; the bare and 'dftu.' namespaces belong to "
                      "the host">(dstd::StringView(name ? name : "(null)"),
                                  plugin, plugin));
    }
    auto registered = register_provider_vtable(
        name, vt, self, std::move(library), std::move(memory));
    if (!registered) {
        return host_fail(
            ctx, common::to_status(registered.error().condition()),
            dstd::fmt<"provider '{}' refused: {}">(
                dstd::StringView(name), registered.error().message()));
    }
    return DFTU_OK;
}

const void* detail::providers_ext_vtable() { return &g_providers; }

}  // namespace dftracer::utils::plugins::host
