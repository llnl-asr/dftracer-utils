// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_NODE_ADAPTER_H
#define DFTRACER_UTILS_PLUGINS_HOST_NODE_ADAPTER_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/abi/nodes.h>
#include <dftracer/utils/plugins/host/plugin_library.h>
#include <dftracer/utils/plugins/host/plugin_memory.h>

namespace dftracer::utils::plugins::host {

/// Registers `vt`/`self` as the plan node `name`. `vt` is copied. Every cursor
/// and registry entry made from it holds `library`, so the plugin code stays
/// mapped until the last of them is gone; null for a node that is not in a
/// loaded library. Every cursor it opens charges its reservations to `memory`,
/// the pool of the plugin; null gives each cursor a pool of its own. On success
/// `self` is destroyed once through `vt->destroy`, after the node is
/// unregistered and every cursor and plan that holds it is gone; on failure
/// `self` stays with the caller. Refuses a null name or vtable, a vtable of a
/// wrong struct_size or without output_schema and open, and a name already
/// taken.
dstd::Result<void> register_node_vtable(
    dstd::StringView name, const ::dftu_node_vt* vt, void* self,
    dstd::SharedPtr<PluginLibrary> library = nullptr,
    dstd::SharedPtr<PluginMemory> memory = nullptr);

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_NODE_ADAPTER_H
