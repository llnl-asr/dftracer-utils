// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_FILE_FORMAT_SOURCE_H
#define DFTRACER_UTILS_PLUGINS_HOST_FILE_FORMAT_SOURCE_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/abi/file_formats.h>
#include <dftracer/utils/plugins/host/plugin_library.h>
#include <dftracer/utils/plugins/host/plugin_memory.h>

namespace dftracer::utils::plugins::host {

/// Registers `vt`/`self` as the file format `name`, so
/// dftracer::utils::dataframe::LazyFrame::scan_file can open a file with it.
/// `vt` is copied. Every file opened through it, and every cursor scanning one,
/// holds `library`, so the plugin code stays mapped until the last of them is
/// gone; null for a format that is not in a loaded library. Every cursor it
/// opens charges its reservations to `memory`, the pool of the plugin; null
/// gives each cursor a pool of its own. On success `self` is destroyed once
/// through `vt->destroy`, after the format is unregistered and every file it
/// opened is closed; on failure `self` stays with the caller. Refuses a null
/// name or vtable, a vtable of a wrong struct_size or without probe, open,
/// schema, chunks or scan, and a name already taken.
dstd::Result<void> register_file_format_vtable(
    dstd::StringView name, const ::dftu_file_format_vt* vt, void* self,
    dstd::SharedPtr<PluginLibrary> library = nullptr,
    dstd::SharedPtr<PluginMemory> memory = nullptr);

/// Registers `vt`/`self` as the sink `name`. `vt` is copied, and every output
/// bound from it holds `library`, so the plugin code stays mapped while a sink
/// writes. On success `self` is destroyed once through `vt->destroy`, after the
/// sink is unregistered and every output bound from it is gone; on failure
/// `self` stays with the caller. Refuses a null name or vtable, a vtable of a
/// wrong struct_size or without bind, and a name already taken.
dstd::Result<void> register_sink_vtable(
    dstd::StringView name, const ::dftu_sink_vt* vt, void* self,
    dstd::SharedPtr<PluginLibrary> library = nullptr);

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_FILE_FORMAT_SOURCE_H
