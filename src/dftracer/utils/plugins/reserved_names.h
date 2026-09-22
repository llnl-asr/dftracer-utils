#ifndef DFTRACER_UTILS_PLUGINS_RESERVED_NAMES_H
#define DFTRACER_UTILS_PLUGINS_RESERVED_NAMES_H

#include <string_view>

namespace dftracer::utils::plugins {

/// The prefix the host keeps for its own ops, ports and accumulators.
inline constexpr std::string_view HOST_NAME_PREFIX = "dftu.";

/// True when `name` is in the host's own namespace. The plugin ABI is the only
/// way into the op, port and accumulator registries, so this is checked on
/// every name a plugin supplies: its provides/consumes lists,
/// dftu_svc_agg::agg_new and dftu_svc_ops::register_op. A plugin naming
/// anything here would shadow a host entry and silently change what every other
/// plugin and the planner resolve. constexpr so a host-registered literal can
/// be checked where it is written.
constexpr bool is_host_namespace(std::string_view name) noexcept {
    return name.substr(0, HOST_NAME_PREFIX.size()) == HOST_NAME_PREFIX;
}

/// True when `name` has the `<plugin>.<name>` shape a plugin must use.
constexpr bool is_plugin_qualified(std::string_view name) noexcept {
    const std::size_t dot = name.find('.');
    return dot != std::string_view::npos && dot != 0 && dot + 1 < name.size();
}

/// The gate for a name a plugin registers into the op registry. Every host op
/// now lives under `dftu.` (`dftu.series.add`, `dftu.frame.select`, ...), so
/// the bare namespace (`add`, `sum`, ...) is no longer host-owned in fact - but
/// it stays reserved anyway: a plugin op must be `<plugin>.<name>`, keeping the
/// bare namespace free for a future host op without a further ABI break.
constexpr bool refuse_plugin_op_name(std::string_view name) noexcept {
    return is_host_namespace(name) || !is_plugin_qualified(name);
}

/// Null-tolerant overloads for the C ABI, where a name may be absent.
constexpr bool is_host_namespace(const char* name) noexcept {
    return name != nullptr && is_host_namespace(std::string_view{name});
}
constexpr bool refuse_plugin_op_name(const char* name) noexcept {
    return name == nullptr || refuse_plugin_op_name(std::string_view{name});
}

static_assert(is_host_namespace("dftu.hash.fnv1a"));
static_assert(!is_host_namespace("com.example.dftu.stats"));
static_assert(!is_host_namespace(static_cast<const char*>(nullptr)));
static_assert(refuse_plugin_op_name("add"));
static_assert(refuse_plugin_op_name("dftu.hash.fnv1a"));
static_assert(!refuse_plugin_op_name("com.example.myop"));

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_RESERVED_NAMES_H
