// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_BUILD_HOST_H
#define DFTRACER_UTILS_PLUGINS_BUILD_HOST_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/state_registry.h>

#include <utility>

namespace dftracer::utils::plugins {

/// The host a plugin factory receives. It answers only the registration
/// groups - DFTU_SVC_OPS, DFTU_SVC_AGG, DFTU_SVC_PORTS, DFTU_SVC_PROVIDERS,
/// DFTU_SVC_NODES -
/// and, inside those,
/// only the registration slots. Anything that needs a scan (a fold, a batch,
/// the intern table, the runtime) does not exist yet at load, so every other
/// service returns NULL and records the denial: the loader then fails the load
/// naming it, rather than letting the factory dereference a host that is not
/// there. What a factory may do is enforced here, not documented.
class BuildHost {
   public:
    explicit BuildHost(dstd::String plugin_name);

    /// Handed to the factory; valid only for that call.
    dftu_plugin_host* host() { return &host_; }

    /// The first service the factory was denied, empty when it stayed inside
    /// the registration surface.
    const dstd::String& denied() const { return denied_; }

    const dstd::String& plugin_name() const { return plugin_name_; }

    /// Record `what` as denied; only the first is kept, so the loader reports
    /// the call that first stepped outside the build phase.
    void deny(const char* what);

    /// The state types the factory registered, moved out once it returns.
    StateRegistry take_states() { return std::move(states_); }

    StateRegistry& states() { return states_; }

    /// The op names the factory registered with dftu_op_register, moved out
    /// once it returns. The loader must unregister each of these before it
    /// dlcloses the plugin, or the registry keeps a dangling name and fn.
    dstd::Vector<dstd::String> take_registered_ops() {
        return std::move(registered_ops_);
    }

    dstd::Vector<dstd::String>& registered_ops() { return registered_ops_; }

    /// The provider names the factory registered with
    /// dftu_svc_providers::register_provider, moved out once it returns. The
    /// loader must unregister each of these before it dlcloses the plugin, or
    /// the provider registry keeps a dangling vt/self.
    dstd::Vector<dstd::String> take_registered_providers() {
        return std::move(registered_providers_);
    }

    dstd::Vector<dstd::String>& registered_providers() {
        return registered_providers_;
    }

    /// The node names the factory registered with
    /// dftu_svc_nodes::register_node; unregistered before dlclose, as the
    /// providers are.
    dstd::Vector<dstd::String> take_registered_nodes() {
        return std::move(registered_nodes_);
    }

    dstd::Vector<dstd::String>& registered_nodes() { return registered_nodes_; }

    /// The index extension names registered with
    /// dftu_svc_index::register_extension; unregistered before dlclose, as the
    /// providers are.
    dstd::Vector<dstd::String> take_registered_index_extensions() {
        return std::move(registered_index_extensions_);
    }

    dstd::Vector<dstd::String>& registered_index_extensions() {
        return registered_index_extensions_;
    }

   private:
    dstd::String plugin_name_;
    dstd::String denied_;
    StateRegistry states_;
    dstd::Vector<dstd::String> registered_ops_;
    dstd::Vector<dstd::String> registered_providers_;
    dstd::Vector<dstd::String> registered_nodes_;
    dstd::Vector<dstd::String> registered_index_extensions_;
    dftu_plugin_host host_{};
};

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_BUILD_HOST_H
