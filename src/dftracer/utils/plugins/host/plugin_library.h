// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_LIBRARY_H
#define DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_LIBRARY_H

#include <dftracer/utils/plugins/abi/plugin.h>
#include <dlfcn.h>

namespace dftracer::utils::plugins::host {

/// A dlopen handle of a plugin library, and the plugin instance its factory
/// made, closed when the last holder is gone: the plugin's own destroy runs
/// first, then dlclose.
/// The host, never the plugin, takes a reference for every object it makes
/// that keeps a plugin function pointer or `self` (a registered node,
/// provider, index extension and the cursors they open), so the library stays
/// mapped while any of them is alive. Declare the reference as the first
/// member of such an object: the destructor then calls into the plugin first
/// and the reference drops last. The plugin's destroy therefore runs after the
/// last object the plugin made is gone, so those objects may use the plugin's
/// global state in their own destroy.
class PluginLibrary {
   public:
    explicit PluginLibrary(void* handle) noexcept : handle_(handle) {}
    PluginLibrary(const PluginLibrary&) = delete;
    PluginLibrary& operator=(const PluginLibrary&) = delete;
    ~PluginLibrary() {
        if (plugin_ && plugin_->destroy) plugin_->destroy(plugin_->self);
        if (handle_) dlclose(handle_);
    }

    /// The plugin instance whose destroy runs before the library closes.
    void adopt(::dftu_plugin* plugin) noexcept { plugin_ = plugin; }

    void* handle() const noexcept { return handle_; }

   private:
    void* handle_;
    ::dftu_plugin* plugin_ = nullptr;
};

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_PLUGIN_LIBRARY_H
