// SPDX-License-Identifier: MIT
/* Test-only plugin authored through the fluent C++ builder: a fold, an op and
 * a mergeable state type registered from one factory, which is the shape a
 * single make_plugin<Slice> cannot express.
 */

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstdint>
#include <cstdlib>

using namespace dftracer::utils;

using namespace dftracer::utils::plugins;

namespace {

constexpr const char* OP_NAME = "builder_plugin.double_rows";
constexpr const char* STATE_NAME = "builder_plugin.total_dur";

std::int64_t double_rows_sdk(const dataframe::ColumnView& v) {
    return 2 * v.size();
}

// Nothing but a running total, so the test can compare the state's answer
// against the fold's and see both halves of one factory work.
struct TotalDur {
    std::uint64_t total = 0;

    void update(dataframe::FrameView df) {
        const dstd::Optional<dataframe::ColumnView> dur = df.column("dur");
        if (!dur) return;
        const dstd::Result<dataframe::Column> flat = dur->flat();
        if (!flat.has_value()) return;
        const dataframe::ColumnView view{flat->get()};
        const dstd::Span<const std::uint64_t> v = view.values<std::uint64_t>();
        for (const std::uint64_t x : v) total += x;
    }

    void merge(TotalDur& other) { total += other.total; }
    std::uint64_t bytes() const { return sizeof(TotalDur); }
    dstd::String serialize() const { return dstd::to_string(total); }
    static TotalDur* deserialize(dstd::StringView b) {
        auto* s = new TotalDur();
        s->total = std::strtoull(dstd::String(b).c_str(), nullptr, 10);
        return s;
    }
    dstd::String finalize() { return dstd::to_string(total); }
};

struct RowCount {
    std::uint64_t rows = 0;

    explicit RowCount(const Config&) {}

    void step(dataframe::FrameView df, Host) {
        rows += static_cast<std::uint64_t>(df.row_count());
    }
    void merge(RowCount& other) { rows += other.rows; }
    dstd::Result<void> finalize(Host h) {
        const dstd::String s = dstd::to_string(rows);
        return h.emit_result("builder_plugin.rows", s);
    }
};

}  // namespace

DFTU_PLUGIN("builder_plugin", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.fold<RowCount>();
                p.op(OpSpec::series_i64<&double_rows_sdk>(OP_NAME));
                p.state<TotalDur>(STATE_NAME);
                return {};
            })
