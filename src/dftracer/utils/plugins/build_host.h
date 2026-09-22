#ifndef DFTRACER_UTILS_PLUGINS_BUILD_HOST_H
#define DFTRACER_UTILS_PLUGINS_BUILD_HOST_H

#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/state_registry.h>

#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::plugins {

/// The host a plugin factory receives. It answers only the registration
/// groups - DFTU_SVC_OPS, DFTU_SVC_AGG, DFTU_SVC_PORTS, DFTU_SVC_PROVIDERS -
/// and, inside those,
/// only the registration slots. Anything that needs a scan (a fold, a batch,
/// the intern table, the runtime) does not exist yet at load, so every other
/// service returns NULL and records the denial: the loader then fails the load
/// naming it, rather than letting the factory dereference a host that is not
/// there. What a factory may do is enforced here, not documented.
class BuildHost {
   public:
    explicit BuildHost(std::string plugin_name);

    /// Handed to the factory; valid only for that call.
    dftu_plugin_host* host() { return &host_; }

    /// The first service the factory was denied, empty when it stayed inside
    /// the registration surface.
    const std::string& denied() const { return denied_; }

    const std::string& plugin_name() const { return plugin_name_; }

    /// Record `what` as denied; only the first is kept, so the loader reports
    /// the call that first stepped outside the build phase.
    void deny(const char* what);

    /// The state types the factory registered, moved out once it returns.
    StateRegistry take_states() { return std::move(states_); }

    StateRegistry& states() { return states_; }

    /// The op names the factory registered with dftu_op_register, moved out
    /// once it returns. The loader must unregister each of these before it
    /// dlcloses the plugin, or the registry keeps a dangling name and fn.
    std::vector<std::string> take_registered_ops() {
        return std::move(registered_ops_);
    }

    std::vector<std::string>& registered_ops() { return registered_ops_; }

    /// The provider names the factory registered with
    /// dftu_svc_providers::register_provider, moved out once it returns. The
    /// loader must unregister each of these before it dlcloses the plugin, or
    /// the provider registry keeps a dangling vt/self.
    std::vector<std::string> take_registered_providers() {
        return std::move(registered_providers_);
    }

    std::vector<std::string>& registered_providers() {
        return registered_providers_;
    }

   private:
    std::string plugin_name_;
    std::string denied_;
    StateRegistry states_;
    std::vector<std::string> registered_ops_;
    std::vector<std::string> registered_providers_;
    dftu_plugin_host host_{};
};

}  // namespace dftracer::utils::plugins

#endif  // DFTRACER_UTILS_PLUGINS_BUILD_HOST_H
