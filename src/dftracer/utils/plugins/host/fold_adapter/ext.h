// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_FOLD_ADAPTER_EXT_H
#define DFTRACER_UTILS_PLUGINS_HOST_FOLD_ADAPTER_EXT_H

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/agg_spill.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/host_status.h>
#include <dftracer/utils/plugins/host/plugin_library.h>
#include <dftracer/utils/plugins/host/plugin_memory.h>

#include <cstdint>
#include <string>
#include <vector>

// Private seam shared by the fold_adapter extension translation units
// (fold_adapter.cpp core plus fold_adapter/{agg,result,ops}.cpp). Each ext TU
// defines its own file-static g_* vtable and exposes it through the detail::
// accessor below; host_get_service in the core TU calls the accessors. The
// remaining declarations are the free helpers referenced across more than one
// of those TUs.
namespace dftracer::utils::plugins::host {

// Per-slice engine-backed aggregation accumulator (dft.ext.agg). value_names
// are the distinct value/by columns the specs reference, ordered so each spec's
// value_col/by_col indexes into it. `spiller` bounds the live group map to the
// scan's memory budget, so a high-cardinality key does not grow `state`
// without limit; it holds the spilled runs and drains them at finalize.
struct AggAccum {
    dstd::String name;
    dstd::Vector<dstd::String> key_names;
    dstd::Vector<dstd::String> value_names;
    dataframe::AggStatePtr state;
    dataframe::AggSpiller spiller;
};

inline dstd::StringView resolve_id(dftracer::utils::dstd::StringIntern& intern,
                                   dftu_str id) {
    if (id == DFTU_STR_NONE ||
        id >= dftracer::utils::dstd::StringIntern::FAST_CAPACITY)
        return {};
    return intern.resolve(id);
}

namespace detail {

// Each returns its extension's stateless host vtable, owned file-static by the
// TU that implements that extension's trampolines.
const void* agg_ext_vtable();
const void* result_ext_vtable();
const void* ops_ext_vtable();
const void* providers_ext_vtable();
const void* file_formats_ext_vtable();
const void* nodes_ext_vtable();
const void* index_ext_vtable();
const void* task_ext_vtable();
/// The dftu.svc.ports table of a fold's host.
const void* ports_ext_vtable();
const void* fs_ext_vtable();
const void* fileio_ext_vtable();
const void* json_ext_vtable();
const void* yaml_ext_vtable();
const void* memory_ext_vtable();
/// The dftu.svc.store table: the index root's store of the scan's trace
/// folder, in the plugin's own namespaces.
const void* store_ext_vtable();
/// Test only: makes the next store flush a fold runs fail, so a test can prove
/// the error reaches the run's result.
void fail_next_store_flush_for_test();
/// The dftu.svc.memory table for the host of a provider or node cursor.
const void* cursor_memory_ext_vtable();

/// The table of a service that keeps no state in a fold (io, sketch, ops,
/// providers, nodes, index, fs, json, yaml and file formats), shared by the
/// host of a fold and the host of a cursor; null for any other id.
const void* stateless_service(dstd::StringView ext_id);

/// Logs a plugin's line at `level` (a dftu_log_level) tagged with its name.
void log_plugin_line(dstd::StringView plugin, std::uint8_t level,
                     dstd::StringView msg);

/// dftu_svc_ops::register_op with the plugin-name gate applied. Shared with
/// the build-phase host, whose op registration is the same call. The record
/// keeps `library` alive, so a plan that holds the op keeps the plugin loaded.
dftu_status register_plugin_op(::dftu_ctx* ctx, const ::dftu_op_desc* desc,
                               dstd::SharedPtr<PluginLibrary> library);

/// dftu_svc_ops::register_udf. The record keeps `library` alive, so a plan
/// that holds the call keeps the plugin loaded.
dftu_status register_plugin_udf(::dftu_ctx* ctx, const ::dftu_udf_desc* desc,
                                void* self,
                                dstd::SharedPtr<PluginLibrary> library);

/// dftu_svc_ops::register_udaf. The record keeps `library` alive, so a plan
/// that holds the call keeps the plugin loaded.
dftu_status register_plugin_udaf(::dftu_ctx* ctx, const ::dftu_udaf_desc* desc,
                                 void* self,
                                 dstd::SharedPtr<PluginLibrary> library);

/// dftu_svc_task::run, which uses no host state, so the build-phase host
/// serves it too.
dftu_status task_run(::dftu_ctx* ctx, void* h, ::dftu_task* t);

/// dftu_svc_task::test, cancel, retain and release; they use no host state
/// either.
dftu_status task_test(void* h, const ::dftu_task* t);
void task_cancel(void* h, ::dftu_task* t);
void task_retain(void* h, ::dftu_task* t);
void task_release(void* h, ::dftu_task* t);

/// dftu_svc_providers::register_provider behind the plugin-namespace gate
/// (name_in_plugin_namespace), forwarding into the process-lifetime
/// provider_registry. Shared with the build-phase host, whose provider
/// registration is the same call. `plugin` is the name the plugin declares in
/// its dftu_plugin_info.
dftu_status register_plugin_provider(::dftu_ctx* ctx, dstd::StringView plugin,
                                     dstd::StringView name,
                                     const ::dftu_source_vt* vt, void* self,
                                     dstd::SharedPtr<PluginLibrary> library,
                                     dstd::SharedPtr<PluginMemory> memory);

/// dftu_svc_nodes::register_node with the same plugin-name gate, forwarding
/// into the process-lifetime node registry.
dftu_status register_plugin_node(::dftu_ctx* ctx, dstd::StringView name,
                                 const ::dftu_node_vt* vt, void* self,
                                 dstd::SharedPtr<PluginLibrary> library,
                                 dstd::SharedPtr<PluginMemory> memory);

/// dftu_svc_index::register_extension with the same plugin-name gate,
/// forwarding into the process index extension registry.
dftu_status register_plugin_index_extension(
    ::dftu_ctx* ctx, dstd::StringView name, const ::dftu_index_extension* vt,
    void* self, dstd::SharedPtr<PluginLibrary> library);

/// dftu_svc_file_formats::register_format behind the plugin-namespace gate,
/// forwarding into the process-lifetime file format registry. The format keeps
/// `library` alive, so a plan over a file it opened keeps the plugin mapped.
dftu_status register_plugin_file_format(::dftu_ctx* ctx,
                                        dstd::StringView plugin,
                                        dstd::StringView name,
                                        const ::dftu_file_format_vt* vt,
                                        void* self,
                                        dstd::SharedPtr<PluginLibrary> library,
                                        dstd::SharedPtr<PluginMemory> memory);

/// dftu_svc_file_formats::register_sink behind the plugin-namespace gate,
/// forwarding into the process-lifetime sink registry.
dftu_status register_plugin_sink(::dftu_ctx* ctx, dstd::StringView plugin,
                                 dstd::StringView name,
                                 const ::dftu_sink_vt* vt, void* self,
                                 dstd::SharedPtr<PluginLibrary> library);

}  // namespace detail

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_FOLD_ADAPTER_EXT_H
