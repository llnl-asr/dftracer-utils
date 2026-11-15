// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_ABI_H
#define DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_ABI_H

#include <dftracer/utils/dstd/string.h>

#include <cstdint>

namespace dftracer::utils::plugins::host {

bool abi_compatible(std::uint32_t host_version,
                    std::uint32_t plugin_version) noexcept;

dstd::String abi_version_string(std::uint32_t version);

enum class PluginStruct : std::uint8_t {
    PluginInfo,
    Plugin,
    StateDesc,
    NodeVt,
    CursorVt,
    SourceVt,
    FileFormatVt,
    SinkVt,
    IndexExtension,
    JobDesc,
    UdfDesc,
    UdafDesc,
};

bool plugin_struct_size_ok(PluginStruct id, std::uint32_t src_size,
                           std::uint32_t host_size) noexcept;
std::uint32_t plugin_struct_min_size(PluginStruct id) noexcept;
const char* plugin_struct_name(PluginStruct id) noexcept;

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_ABI_H
