// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/plugins/host/file_format_source.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/host/reserved_names.h>

namespace dftracer::utils::plugins::host {
namespace {

// Registration is a load-time concern, as for providers: a format registered
// mid-scan reaches no plan built before it existed, so the scan-time host does
// not register formats or sinks.
dftu_status host_file_formats_register_format(::dftu_ctx* ctx, void*,
                                              const char* name,
                                              const ::dftu_file_format_vt*,
                                              void*) {
    return host_fail(
        ctx, DFTU_E_UNSUPPORTED,
        dstd::fmt<"file format '{}' refused: register from the plugin factory, "
                  "which runs before the scan; the scan-time host does not "
                  "register file formats">(
            dstd::StringView(name ? name : "(null)")));
}

dftu_status host_file_formats_register_sink(::dftu_ctx* ctx, void*,
                                            const char* name,
                                            const ::dftu_sink_vt*, void*) {
    return host_fail(
        ctx, DFTU_E_UNSUPPORTED,
        dstd::fmt<"sink '{}' refused: register from the plugin factory, which "
                  "runs before the scan; the scan-time host does not register "
                  "sinks">(dstd::StringView(name ? name : "(null)")));
}

const ::dftu_svc_file_formats g_file_formats = {
    sizeof(::dftu_svc_file_formats), host_file_formats_register_format,
    host_file_formats_register_sink};

}  // namespace

dftu_status detail::register_plugin_file_format(
    ::dftu_ctx* ctx, dstd::StringView plugin, dstd::StringView name,
    const ::dftu_file_format_vt* vt, void* self,
    dstd::SharedPtr<PluginLibrary> library,
    dstd::SharedPtr<PluginMemory> memory) {
    if (!name_in_plugin_namespace(name, plugin)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"file format '{}' refused: a file format name must be "
                      "'{}' or '{}.<name>'; the bare and 'dftu.' namespaces "
                      "belong to the host">(name, plugin, plugin));
    }
    auto registered = register_file_format_vtable(
        name, vt, self, std::move(library), std::move(memory));
    if (!registered) {
        return host_fail(ctx, common::to_status(registered.error().condition()),
                         dstd::fmt<"file format '{}' refused: {}">(
                             name, registered.error().message()));
    }
    return DFTU_OK;
}

dftu_status detail::register_plugin_sink(
    ::dftu_ctx* ctx, dstd::StringView plugin, dstd::StringView name,
    const ::dftu_sink_vt* vt, void* self,
    dstd::SharedPtr<PluginLibrary> library) {
    if (!name_in_plugin_namespace(name, plugin)) {
        return host_fail(
            ctx, DFTU_E_INVALID,
            dstd::fmt<"sink '{}' refused: a sink name must be '{}' or "
                      "'{}.<name>'; the bare and 'dftu.' namespaces belong to "
                      "the host">(name, plugin, plugin));
    }
    auto registered = register_sink_vtable(name, vt, self, std::move(library));
    if (!registered) {
        return host_fail(ctx, common::to_status(registered.error().condition()),
                         dstd::fmt<"sink '{}' refused: {}">(
                             name, registered.error().message()));
    }
    return DFTU_OK;
}

const void* detail::file_formats_ext_vtable() { return &g_file_formats; }

}  // namespace dftracer::utils::plugins::host
