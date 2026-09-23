#ifndef DFTRACER_UTILS_PLUGINS_STATE_REGISTRY_H
#define DFTRACER_UTILS_PLUGINS_STATE_REGISTRY_H

#include <dftracer/utils/plugins/abi.h>

#include <string>
#include <vector>

namespace dftracer::utils::plugins {

/// One dftu_state_desc a plugin's factory registered, with the `self` its
/// callbacks receive. The descriptor is copied at registration, so the one the
/// plugin passes need not outlive the call; `name` owns the key and `self`
/// must outlive the plugin.
struct RegisteredState {
    std::string name;
    dftu_state_desc desc{};
    void* self = nullptr;
};

/// A plugin's registered state types, in registration order. Settled by
/// Plugins::Builder::build() and read by every PluginFold slice.
using StateRegistry = std::vector<RegisteredState>;

/// Copy `desc` into `out` under the ABI's registration rules; returns 0 on
/// success and non-zero (having logged the reason) otherwise.
int register_state_into(StateRegistry& out, const dftu_state_desc* desc,
                        void* self, const std::string& plugin_name);

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_STATE_REGISTRY_H
