// SPDX-License-Identifier: MIT
/* Test-only plugin that declares the config keys it reads, so the host can
 * validate a caller's config instead of silently ignoring what it does not
 * recognize.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/plugins/plugin.h>

#include <array>
#include <cstdint>

using namespace dftracer::utils;

using namespace dftracer::utils::plugins;

namespace {

struct Counted {
    std::int64_t stride = 1;
    std::uint64_t rows = 0;

    explicit Counted(const Config& cfg) : stride(cfg.get_int("stride", 1)) {}

    static auto config_keys() {
        return std::array<ConfigKey, 2>{
            ConfigKey{"stride", ValueKind::I64, false, "rows counted per row"},
            ConfigKey{"label", ValueKind::Str, true, "name of the result"}};
    }

    void step(dataframe::FrameView df, Host) {
        rows += static_cast<std::uint64_t>(df.row_count()) *
                static_cast<std::uint64_t>(stride);
    }
    void merge(Counted& other) { rows += other.rows; }
    dstd::Result<void> finalize(Host h) {
        return h.emit_result("config_keys_plugin.rows", dstd::to_string(rows));
    }
};

}  // namespace

DFTU_PLUGIN("config_keys_plugin", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.fold<Counted>();
                return {};
            })
