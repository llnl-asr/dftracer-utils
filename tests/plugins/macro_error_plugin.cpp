// SPDX-License-Identifier: MIT
// A DFTU_PLUGIN body that returns an error: the load must fail with that
// error's condition and message, and the plugin must not load at all.

#include <dftracer/utils/plugins/plugin.h>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

DFTU_PLUGIN("macro_error_plugin", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                (void)p;
                return invalid_argument("macro_plugin: refusing to load");
            })
