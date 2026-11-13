// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_PROVIDER_SOURCE_H
#define DFTRACER_UTILS_PLUGINS_HOST_PROVIDER_SOURCE_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/abi/providers.h>
#include <dftracer/utils/plugins/host/plugin_library.h>
#include <dftracer/utils/plugins/host/plugin_memory.h>

namespace dftracer::utils::plugins::host {

/// Adapts a dftu_source_vt/self pair as a dataframe Source, for a test that
/// drives the scan() C-ABI marshaling (dftu_scan_request, the out_pushed array,
/// projection verification) against a hand-written vtable without the plugin
/// system. `vt` is copied; `self` must outlive every scan opened against the
/// returned Source, and is never destroyed by it. The cursors it opens charge
/// their reservations to `memory`, or to a pool of their own when it is null.
dstd::SharedPtr<dataframe::Source> make_provider_source(
    const ::dftu_source_vt& vt, void* self,
    dstd::SharedPtr<PluginMemory> memory = nullptr);

/// Registers `vt`/`self` as the provider `name`. `vt` is copied. Every Source,
/// cursor and registry entry made from it holds `library`, so the plugin code
/// stays mapped until the last of them is gone; null for a provider that is
/// not in a loaded library. Every cursor it opens charges its reservations to
/// `memory`, the pool of the plugin; null gives each cursor a pool of its own.
/// On success
/// `self` is destroyed once through `vt.destroy`, after the provider is
/// unregistered and every Source and cursor made from it is gone; on failure
/// `self` stays with the caller. Refuses a null name or vtable, a vtable of a
/// wrong struct_size and a name already taken.
dstd::Result<void> register_provider_vtable(
    dstd::StringView name, const ::dftu_source_vt* vt, void* self,
    dstd::SharedPtr<PluginLibrary> library = nullptr,
    dstd::SharedPtr<PluginMemory> memory = nullptr);

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_PROVIDER_SOURCE_H
