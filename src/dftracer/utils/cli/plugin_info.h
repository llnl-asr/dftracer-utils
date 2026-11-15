// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CLI_PLUGIN_INFO_H
#define DFTRACER_UTILS_CLI_PLUGIN_INFO_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/host/plugins.h>

#include <cstdio>

// The one rendering of a loaded plugin's facts, shared by `dftracer_run
// --describe` and `dftracer_plugin check`, so both report the same lines.
namespace dftracer::utils::cli {

/// One plugin's facts as text, one fact per line, ending with a blank line:
/// the path it was loaded from, the name and version it declares, the ABI
/// version this host speaks, and every kind list it registered.
dstd::String describe_plugin(const plugins::host::Plugins::PluginInfo& info);

/// @ref describe_plugin written to `out`.
void print_plugin_info(std::FILE* out,
                       const plugins::host::Plugins::PluginInfo& info);

}  // namespace dftracer::utils::cli

#endif  // DFTRACER_UTILS_CLI_PLUGIN_INFO_H
