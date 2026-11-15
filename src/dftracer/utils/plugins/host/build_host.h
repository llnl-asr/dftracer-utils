// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_BUILD_HOST_H
#define DFTRACER_UTILS_PLUGINS_HOST_BUILD_HOST_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/plugin_library.h>
#include <dftracer/utils/plugins/host/plugin_memory.h>
#include <dftracer/utils/plugins/host/state_registry.h>

#include <utility>

namespace dftracer::utils::plugins::host {

/// The host a plugin factory receives. It answers only the registration
/// groups - DFTU_SVC_OPS, DFTU_SVC_AGG, DFTU_SVC_PORTS, DFTU_SVC_PROVIDERS,
/// DFTU_SVC_NODES, DFTU_SVC_INDEX, DFTU_SVC_FILE_FORMATS -
/// and, inside those,
/// only the registration slots, plus dftu_svc_task::run, which uses no host
/// state and so stays valid after the factory returns. Anything that needs a
/// scan (a fold, a batch, the intern table, the runtime) does not exist yet at
/// load, so every other service returns NULL and records the denial: the loader
/// then fails the load naming it, rather than letting the factory dereference a
/// host that is not there. What a factory may do is enforced here, not
/// documented.
class BuildHost {
   public:
    /// `library` is the plugin library being loaded; every object the factory
    /// registers through this host keeps a reference to it. Null for a
    /// plugin that was not loaded from a library.
    explicit BuildHost(dstd::String plugin_name,
                       dstd::SharedPtr<PluginLibrary> library = nullptr);

    const dstd::SharedPtr<PluginLibrary>& library() const { return library_; }

    /// The pool of the plugin being loaded. The providers and nodes the
    /// factory registers charge their cursors to it, and the loaded plugin
    /// keeps it for its folds.
    const dstd::SharedPtr<PluginMemory>& memory() const { return memory_; }

    /// Handed to the factory; valid only for that call.
    dftu_plugin_host* host() { return &host_; }

    /// The first service the factory was denied, empty when it stayed inside
    /// the registration surface.
    const dstd::String& denied() const { return denied_; }

    const dstd::String& plugin_name() const { return plugin_name_; }

    /// The name the plugin declares in its dftu_plugin_info, or the load-path
    /// stem when it declares none. This is the namespace providers, file
    /// formats and sinks register under (name_in_plugin_namespace).
    const dstd::String& declared_name() const {
        return declared_name_.empty() ? plugin_name_ : declared_name_;
    }

    /// The declared name arrives with dftu_plugin_info, which the loader reads
    /// before it constructs the factory's host.
    void set_declared_name(dstd::String name) {
        declared_name_ = std::move(name);
    }

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

    /// The UDF names the factory registered with
    /// dftu_svc_ops::register_udf; unregistered before dlclose, as the ops
    /// are.
    dstd::Vector<dstd::String> take_registered_udfs() {
        return std::move(registered_udfs_);
    }

    dstd::Vector<dstd::String>& registered_udfs() { return registered_udfs_; }

    /// The UDAF names the factory registered with
    /// dftu_svc_ops::register_udaf; unregistered before dlclose, as the UDFs
    /// are.
    dstd::Vector<dstd::String> take_registered_udafs() {
        return std::move(registered_udafs_);
    }

    dstd::Vector<dstd::String>& registered_udafs() { return registered_udafs_; }

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

    /// The file format names the factory registered with
    /// dftu_svc_file_formats::register_format; unregistered before dlclose, as
    /// the providers are.
    dstd::Vector<dstd::String> take_registered_file_formats() {
        return std::move(registered_file_formats_);
    }

    dstd::Vector<dstd::String>& registered_file_formats() {
        return registered_file_formats_;
    }

    /// The sink names the factory registered with
    /// dftu_svc_file_formats::register_sink; unregistered before dlclose, as
    /// the formats are.
    dstd::Vector<dstd::String> take_registered_sinks() {
        return std::move(registered_sinks_);
    }

    dstd::Vector<dstd::String>& registered_sinks() { return registered_sinks_; }

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
    dstd::SharedPtr<PluginLibrary> library_;
    dstd::String plugin_name_;
    dstd::String declared_name_;
    dstd::SharedPtr<PluginMemory> memory_;
    dstd::String denied_;
    StateRegistry states_;
    dstd::Vector<dstd::String> registered_ops_;
    dstd::Vector<dstd::String> registered_udfs_;
    dstd::Vector<dstd::String> registered_udafs_;
    dstd::Vector<dstd::String> registered_providers_;
    dstd::Vector<dstd::String> registered_nodes_;
    dstd::Vector<dstd::String> registered_file_formats_;
    dstd::Vector<dstd::String> registered_sinks_;
    dstd::Vector<dstd::String> registered_index_extensions_;
    dftu_plugin_host host_{};
};

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_BUILD_HOST_H
