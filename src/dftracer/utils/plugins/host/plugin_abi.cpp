// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/plugin_abi.h>

#include <cstddef>

namespace dftracer::utils::plugins::host {

namespace {

constexpr std::uint32_t abi_major(std::uint32_t version) noexcept {
    return version >> 16;
}

constexpr std::uint32_t abi_minor(std::uint32_t version) noexcept {
    return (version >> 8) & 0xFFu;
}

constexpr std::uint32_t abi_patch(std::uint32_t version) noexcept {
    return version & 0xFFu;
}

struct StructLimit {
    const char* name;
    std::uint32_t min_size;
};

// The minimum of each plugin-to-host record is the size before its trailing
// optional callbacks: every member from the minimum to sizeof is optional and
// the host NULL-checks it before a call. An entry whose minimum is sizeof has
// no optional tail.
constexpr StructLimit PLUGIN_STRUCT_LIMITS[] = {
    {"dftu_plugin_info",
     static_cast<std::uint32_t>(offsetof(dftu_plugin_info, version))},
    {"dftu_plugin",
     static_cast<std::uint32_t>(offsetof(dftu_plugin, transform))},
    {"dftu_state_desc",
     static_cast<std::uint32_t>(sizeof(dftu_state_desc))},  // no optional tail
    {"dftu_node_vt",
     static_cast<std::uint32_t>(offsetof(dftu_node_vt, destroy))},
    {"dftu_cursor_vt",
     static_cast<std::uint32_t>(offsetof(dftu_cursor_vt, narrow))},
    {"dftu_source_vt",
     static_cast<std::uint32_t>(offsetof(dftu_source_vt, apply))},
    {"dftu_file_format_vt", static_cast<std::uint32_t>(sizeof(
                                dftu_file_format_vt))},    // no optional tail
    {"dftu_sink_vt",
     static_cast<std::uint32_t>(sizeof(dftu_sink_vt))},    // no optional tail
    {"dftu_index_extension", static_cast<std::uint32_t>(sizeof(
                                 dftu_index_extension))},  // no optional tail
    {"dftu_job_desc",
     static_cast<std::uint32_t>(sizeof(dftu_job_desc))},   // no optional tail
    {"dftu_udf_desc",
     static_cast<std::uint32_t>(offsetof(dftu_udf_desc, free_bound))},
    {"dftu_udaf_desc",
     static_cast<std::uint32_t>(offsetof(dftu_udaf_desc, free_bound))},
    {"dftu_channel_desc",
     static_cast<std::uint32_t>(sizeof(dftu_channel_desc))},  // no tail
};

static_assert(sizeof(PLUGIN_STRUCT_LIMITS) / sizeof(PLUGIN_STRUCT_LIMITS[0]) ==
              13);

}  // namespace

bool abi_compatible(std::uint32_t host_version,
                    std::uint32_t plugin_version) noexcept {
    return abi_major(host_version) == abi_major(plugin_version) &&
           abi_minor(plugin_version) <= abi_minor(host_version);
}

dstd::String abi_version_string(std::uint32_t version) {
    return dstd::fmt<"{}.{}.{}">(abi_major(version), abi_minor(version),
                                 abi_patch(version));
}

bool plugin_struct_size_ok(PluginStruct id, std::uint32_t src_size,
                           std::uint32_t host_size) noexcept {
    return src_size >= plugin_struct_min_size(id) && src_size <= host_size;
}

std::uint32_t plugin_struct_min_size(PluginStruct id) noexcept {
    return PLUGIN_STRUCT_LIMITS[static_cast<std::size_t>(id)].min_size;
}

const char* plugin_struct_name(PluginStruct id) noexcept {
    return PLUGIN_STRUCT_LIMITS[static_cast<std::size_t>(id)].name;
}

}  // namespace dftracer::utils::plugins::host
