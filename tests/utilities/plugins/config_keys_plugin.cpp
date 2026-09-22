/* Test-only plugin that declares the config keys it reads, so the host can
 * validate a caller's config instead of silently ignoring what it does not
 * recognize.
 */

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/plugin.h>

#include <array>
#include <cstdint>
#include <string>

using namespace dftracer::utils::plugins;

namespace {

struct Counted {
    std::int64_t stride = 1;
    std::uint64_t rows = 0;

    explicit Counted(const Config& cfg) : stride(cfg.get_int("stride", 1)) {}

    static auto config_keys() {
        return std::array<dftu_config_key, 2>{
            dftu_config_key{"stride", DFTU_VAL_I64, 0, "rows counted per row"},
            dftu_config_key{"label", DFTU_VAL_STR, 1, "name of the result"}};
    }

    void step(const dftu_dataframe* df, Host) {
        rows += static_cast<std::uint64_t>(dftu_dataframe_num_rows(df)) *
                static_cast<std::uint64_t>(stride);
    }
    void merge(Counted& other) { rows += other.rows; }
    void finalize(Host h) {
        h.emit_result("config_keys_plugin.rows", std::to_string(rows));
    }
};

}  // namespace

extern "C" DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(
    dftu_plugin_host* h, const dftu_value* config) {
    return plugin(h, config).fold<Counted>().build();
}
