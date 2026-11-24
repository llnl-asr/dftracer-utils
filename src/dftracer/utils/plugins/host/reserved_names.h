// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_RESERVED_NAMES_H
#define DFTRACER_UTILS_PLUGINS_HOST_RESERVED_NAMES_H

#include <dftracer/utils/dstd/string.h>

#include <string_view>

namespace dftracer::utils::plugins::host {

/// The prefix the host keeps for its own ops, ports and accumulators.
inline constexpr dstd::StringView HOST_NAME_PREFIX = "dftu.";

/// True when `name` is in the host's own namespace. The plugin ABI is the only
/// way into the op, port and accumulator registries, so this is checked on
/// every name a plugin supplies: its provides/consumes lists,
/// dftu_svc_agg::agg_new and dftu_svc_ops::register_op. A plugin naming
/// anything here would shadow a host entry and silently change what every other
/// plugin and the planner resolve. constexpr so a host-registered literal can
/// be checked where it is written.
constexpr bool is_host_namespace(dstd::StringView name) noexcept {
    return name.starts_with(HOST_NAME_PREFIX);
}

/// True when `name` has the `<plugin>.<name>` shape a plugin must use.
constexpr bool is_plugin_qualified(dstd::StringView name) noexcept {
    const std::size_t dot = name.find('.');
    return dot != dstd::StringView::npos && dot != 0 && dot + 1 < name.size();
}

/// True when `name` belongs to the namespace of the plugin named `plugin`:
/// the bare plugin name itself, or `<plugin>.<name>`. Providers, file formats
/// and sinks use this gate, so a plugin that registers one of each can use its
/// own name for the single one and `<plugin>.<x>` for the rest. The host's
/// `dftu.` prefix stays refused, and so does another plugin's name, so a name
/// never reaches past its own plugin.
constexpr bool name_in_plugin_namespace(dstd::StringView name,
                                        dstd::StringView plugin) noexcept {
    if (plugin.empty() || is_host_namespace(name)) return false;
    if (name == plugin) return true;
    return name.size() > plugin.size() + 1 && name.starts_with(plugin) &&
           name[plugin.size()] == '.';
}

/// The gate for a name a plugin registers into the op registry. Every host op
/// now lives under `dftu.` (`dftu.series.add`, `dftu.frame.select`, ...), so
/// the bare namespace (`add`, `sum`, ...) is no longer host-owned in fact - but
/// it stays reserved anyway: a plugin op must be `<plugin>.<name>`, keeping the
/// bare namespace free for a future host op without a further ABI break.
constexpr bool refuse_plugin_op_name(dstd::StringView name) noexcept {
    return is_host_namespace(name) || !is_plugin_qualified(name);
}

static_assert(is_host_namespace("dftu.hash.fnv1a"));
static_assert(!is_host_namespace("com.example.dftu.stats"));
static_assert(refuse_plugin_op_name("add"));
static_assert(refuse_plugin_op_name("dftu.hash.fnv1a"));
static_assert(!refuse_plugin_op_name("com.example.myop"));

static_assert(name_in_plugin_namespace("toy", "toy"));
static_assert(name_in_plugin_namespace("toy.rows", "toy"));
static_assert(!name_in_plugin_namespace("toyz", "toy"));
static_assert(!name_in_plugin_namespace("toyz.rows", "toy"));
static_assert(!name_in_plugin_namespace("toy.", "toy"));
static_assert(!name_in_plugin_namespace("other.rows", "toy"));
static_assert(!name_in_plugin_namespace("dftu.rows", "toy"));
static_assert(!name_in_plugin_namespace("", "toy"));

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_RESERVED_NAMES_H
