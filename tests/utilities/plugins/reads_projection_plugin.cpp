/* Test-only plugin that records the columns dftu_plugin::reads projects into
 * on_batch. One Slice declares `static reads()`, the other does not; the
 * `declare_reads` config key picks which one the factory builds, so a single
 * .so drives both the declared-projection and the full-frame test cases.
 */

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/plugin.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace dftracer::utils::plugins;

namespace {

// Column names and the args.x sum observed on the first non-empty batch: what
// the test asserts on to prove the frame handed to on_batch was actually
// narrowed (or not) by the declared reads() list.
struct ColumnProbe {
    explicit ColumnProbe(const Config&) {}

    bool seen = false;
    std::int64_t num_columns = -1;
    std::string columns;
    bool has_args_x = false;
    double args_x_sum = 0.0;

    void step(const dftu_dataframe* df, Host) {
        if (seen) return;
        seen = true;
        num_columns = dftu_dataframe_num_columns(df);

        std::vector<std::string> names;
        names.reserve(static_cast<std::size_t>(num_columns));
        for (std::int32_t i = 0; i < num_columns; ++i) {
            const char* n = dftu_dataframe_column_name(df, i);
            names.emplace_back(n ? n : "");
        }
        std::sort(names.begin(), names.end());
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i) columns += ",";
            columns += names[i];
        }

        if (dftu_series* xcol = dftu_dataframe_column(df, "args.x")) {
            has_args_x = true;
            if (dftu_series_type(xcol) == DFTU_TYPE_INT64) {
                const auto* data =
                    static_cast<const std::int64_t*>(dftu_series_data(xcol));
                const std::int64_t rows = dftu_dataframe_num_rows(df);
                for (std::int64_t i = 0; i < rows; ++i)
                    if (!dftu_series_is_null(xcol, i))
                        args_x_sum += static_cast<double>(data[i]);
            }
            dftu_series_free(xcol);
        }
    }

    void merge(ColumnProbe& other) {
        if (seen) return;
        seen = other.seen;
        num_columns = other.num_columns;
        columns = std::move(other.columns);
        has_args_x = other.has_args_x;
        args_x_sum = other.args_x_sum;
    }

    void finalize(Host h) {
        h.emit_result("reads_probe.num_columns", std::to_string(num_columns));
        h.emit_result("reads_probe.columns", columns);
        h.emit_result("reads_probe.has_args_x", has_args_x ? "1" : "0");
        h.emit_result("reads_probe.args_x_sum", std::to_string(args_x_sum));
    }
};

struct DeclaredReadsSlice : ColumnProbe {
    using ColumnProbe::ColumnProbe;

    static auto reads() {
        return std::array<const char*, 3>{"dur", "cat", "args.x"};
    }
};

struct UndeclaredReadsSlice : ColumnProbe {
    using ColumnProbe::ColumnProbe;
};

}  // namespace

extern "C" DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(
    dftu_plugin_host* h, const dftu_value* config) {
    Config cfg(config);
    if (cfg.get_bool("declare_reads", false))
        return plugin(h, config).fold<DeclaredReadsSlice>().build();
    return plugin(h, config).fold<UndeclaredReadsSlice>().build();
}
