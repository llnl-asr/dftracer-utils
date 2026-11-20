// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_PROVIDER_SOURCE_H
#define DFTRACER_UTILS_PLUGINS_HOST_PROVIDER_SOURCE_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi/providers.h>
#include <dftracer/utils/plugins/host/plugin_library.h>
#include <dftracer/utils/plugins/host/plugin_memory.h>

namespace dftracer::utils::plugins::host {

class CursorHost;

/// A dataframe::Cursor over one dftu_cursor_vt/self a plugin scan handed back,
/// converting each frame it yields and checking every one of them against
/// `expected_projection` (when non-empty: exactly those columns, in that order)
/// and `expected_types` (a non-Unknown declared type the frame must match). It
/// owns `self`, destroying it through vt->destroy once the cursor is gone,
/// awaits the task a callback leaves pending, and keeps `source` alive, so a
/// plugin's state outlives the cursor. `vt` must be a valid cursor vtable and
/// `self` must belong to the caller until this call. With
/// DFTRACER_UTILS_PLUGIN_CHECK on, every frame it yields is validated and a
/// broken one fails the scan with an error naming `label` (who made the frames
/// and the callback that handed one back, such as "provider 'a.b' next") and
/// the rule. `owner` is the registered name the cursor's run counters are
/// added under, and `counters` the run they are added to, or null.
dstd::UniquePtr<dataframe::Cursor> make_plugin_cursor(
    const ::dftu_cursor_vt* vt, void* self, dstd::UniquePtr<CursorHost> host,
    dstd::Vector<dstd::String> expected_projection,
    dstd::Vector<dataframe::TypeId> expected_types,
    dstd::SharedPtr<const dataframe::Source> source, dstd::String label = {},
    dstd::String owner = {},
    dstd::SharedPtr<dataframe::RunCounters> counters = nullptr);

/// The dataframe::Pushed one dftu_pushed value means. A value the ABI does not
/// define reads as No, which is always sound: the engine re-applies.
dataframe::Pushed plugin_pushed(std::int32_t value);

/// The declared type of each column of `projection`, in that order, or of the
/// whole schema when `projection` is empty. A name the schema does not declare
/// reads as Unknown, so the frame check skips it.
dstd::Vector<dataframe::TypeId> declared_types(
    const dataframe::Schema& declared,
    dstd::Span<const dstd::String> projection);

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
