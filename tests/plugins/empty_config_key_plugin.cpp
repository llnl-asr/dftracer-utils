// SPDX-License-Identifier: MIT
/* Test-only plugin that declares a config key with an empty name, which the
 * host must refuse at load rather than drop.
 */

#include <dftracer/utils/plugins/plugin.h>

#include <array>

using namespace dftracer::utils;

using namespace dftracer::utils::plugins;

namespace {

struct Unnamed {
    explicit Unnamed(const Config&) {}

    static auto config_keys() {
        return std::array<ConfigKey, 2>{
            ConfigKey{"", ValueKind::I64, false, "a key with no name"},
            ConfigKey{"stride", ValueKind::I64, false, "rows per row"}};
    }

    void step(dataframe::FrameView, Host) {}
    void merge(Unnamed&) {}
    void finalize(Host) {}
};

}  // namespace

DFTU_PLUGIN("empty_config_key_plugin", "1.0",
            [](PluginBuilder& p, const Config&) -> dstd::Result<void> {
                p.fold<Unnamed>();
                return {};
            })
