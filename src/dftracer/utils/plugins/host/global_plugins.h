// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PLUGINS_HOST_GLOBAL_PLUGINS_H
#define DFTRACER_UTILS_PLUGINS_HOST_GLOBAL_PLUGINS_H

#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

// The plugins loaded for the whole process, by path: the ones the environment
// names, and the ones a caller loads and unloads. A path is loaded once, by
// its canonical form, and the set it makes stays registered until it is
// unloaded or the process ends.
namespace dftracer::utils::plugins::host {

class Plugins;

/// Registers the built-in trace reader, and loads once the plugins named by
/// the DFTRACER_UTILS_PLUGINS environment variable (see
/// @ref dftracer::utils::common::constants::plugins::PLUGINS_ENV). Every entry
/// point that reads trace files calls it first. Thread safe. A path that does
/// not load is an error that names it; the plugins loaded before it stay
/// loaded, and the next call tries again the paths that are not loaded yet, so
/// a file can be fixed and the process go on.
dstd::Result<void> ensure_global_plugins();

/// Loads the plugin at `path` into the process, or joins it when it is already
/// loaded, as `ensure_global_plugins` does for the environment's plugins. Every
/// successful call must be matched by one @ref unload_plugin. Any error of the
/// load names the path.
dstd::Result<void> load_plugin(dstd::StringView path);

/// Ends one @ref load_plugin of `path`. The plugin is dropped, and its formats
/// unregistered, when the last load of it ends, unless the environment loaded
/// it: that one stays for the process. An InvalidArgument error, and the load
/// kept, when a plan of a scan through one of its formats is still open; a
/// NotFound error for a path that is not loaded.
dstd::Result<void> unload_plugin(dstd::StringView path);

/// Calls `use` with the global set that holds `path`, with the lock held, and
/// returns true; false when none does. A @ref Plugins build of a path that is
/// held shares the held entry through this, instead of registering it again.
bool with_global_plugin(dstd::StringView path,
                        dstd::Function<void(const Plugins&)> use);

}  // namespace dftracer::utils::plugins::host

#endif  // DFTRACER_UTILS_PLUGINS_HOST_GLOBAL_PLUGINS_H
