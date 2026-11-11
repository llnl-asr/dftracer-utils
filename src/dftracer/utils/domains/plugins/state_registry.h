// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DOMAINS_PLUGINS_STATE_REGISTRY_H
#define DFTRACER_UTILS_DOMAINS_PLUGINS_STATE_REGISTRY_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>

namespace dftracer::utils::domains::plugins {

/// One dftu_state_desc a plugin's factory registered, with the `self` its
/// callbacks receive. The descriptor is copied at registration, so the one the
/// plugin passes need not outlive the call; `name` owns the key and `self`
/// must outlive the plugin.
struct RegisteredState {
    dstd::String name;
    dftu_state_desc desc{};
    void* self = nullptr;
};

/// A plugin's registered state types, in registration order. Settled by
/// Plugins::Builder::build() and read by every PluginFold slice.
using StateRegistry = dstd::Vector<RegisteredState>;

/// Copy `desc` into `out` under the ABI's registration rules; returns 0 on
/// success and non-zero (having logged the reason) otherwise.
int register_state_into(StateRegistry& out, const dftu_state_desc* desc,
                        void* self, const dstd::String& plugin_name);

}  // namespace dftracer::utils::domains::plugins

#endif  // DFTRACER_UTILS_DOMAINS_PLUGINS_STATE_REGISTRY_H
