// SPDX-License-Identifier: MIT
/* Test-only plugin that records the columns the host projects into
 * on_batch. One Slice declares `static reads()`, the other does not; the
 * `declare_reads` config key picks which one the factory builds, so a single
 * .so drives both the declared-projection and the full-frame test cases.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>

#include <algorithm>
#include <array>
#include <cstdint>

using namespace dftracer::utils;

using namespace dftracer::utils::plugins;

namespace {

// Column names and the args.x sum observed on the first non-empty batch: what
// the test asserts on to prove the frame handed to on_batch was actually
// narrowed (or not) by the declared reads() list.
struct ColumnProbe {
    explicit ColumnProbe(const Config&) {}

    bool seen = false;
    std::int64_t num_columns = -1;
    dstd::String columns;
    bool has_args_x = false;
    double args_x_sum = 0.0;

    void step(dataframe::FrameView df, Host) {
        if (seen) return;
        seen = true;
        num_columns = df.column_count();

        dstd::Vector<dstd::String> names;
        names.reserve(static_cast<std::size_t>(num_columns));
        for (std::int32_t i = 0; i < num_columns; ++i)
            names.emplace_back(df.name(i));
        std::sort(names.begin(), names.end());
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i) columns += ",";
            columns += names[i];
        }

        dstd::Optional<dataframe::ColumnView> xcol = df.column("args.x");
        if (!xcol) xcol = df.column("x");
        if (xcol) {
            has_args_x = true;
            const dstd::Result<dataframe::Column> flat = xcol->flat();
            if (flat.has_value()) {
                const dataframe::ColumnView view{flat->get()};
                const dstd::Span<const std::int64_t> data =
                    view.values<std::int64_t>();
                for (std::int64_t i = 0;
                     i < static_cast<std::int64_t>(data.size()); ++i) {
                    if (!view.is_null(i))
                        args_x_sum += static_cast<double>(data[i]);
                }
            }
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

    dstd::Result<void> finalize(Host h) {
        DFTRACER_UTILS_TRY_VOID(h.emit_result("reads_probe.num_columns",
                                              dstd::to_string(num_columns)));
        DFTRACER_UTILS_TRY_VOID(h.emit_result("reads_probe.columns", columns));
        DFTRACER_UTILS_TRY_VOID(
            h.emit_result("reads_probe.has_args_x", has_args_x ? "1" : "0"));
        return h.emit_result("reads_probe.args_x_sum",
                             dstd::to_string(args_x_sum));
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

DFTU_PLUGIN("reads_projection_plugin", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                if (config.get_bool("declare_reads", false))
                    p.fold<DeclaredReadsSlice>();
                else
                    p.fold<UndeclaredReadsSlice>();
                return {};
            })
