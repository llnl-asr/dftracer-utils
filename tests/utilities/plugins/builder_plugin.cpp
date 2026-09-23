/* Test-only plugin authored through the fluent C++ builder: a fold, an op and
 * a mergeable state type registered from one factory, which is the shape a
 * single make_plugin<Slice> cannot express.
 */

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace dftracer::utils::plugins;

namespace {

constexpr const char* OP_NAME = "builder_plugin.double_rows";
constexpr const char* STATE_NAME = "builder_plugin.total_dur";

extern "C" std::int64_t double_rows(const dftu_series* v) {
    return 2 * static_cast<std::int64_t>(dftu_series_length(v));
}

const dftu_op_desc OP = {OP_NAME, DFTU_OP_SIG(I64, SERIES, NONE, NONE),
                         reinterpret_cast<const void*>(&double_rows)};

// Nothing but a running total, so the test can compare the state's answer
// against the fold's and see both halves of one factory work.
struct TotalDur {
    std::uint64_t total = 0;

    void update(const dftu_dataframe* df) {
        dftu_series* dur = dftu_dataframe_column(df, "dur");
        if (!dur) return;
        dftu_series* flat = dftu_series_materialize(dur);
        const auto* v =
            static_cast<const std::uint64_t*>(dftu_series_data(flat));
        for (std::int64_t r = 0; r < dftu_series_length(flat); ++r)
            total += v[r];
        dftu_series_free(flat);
        dftu_series_free(dur);
    }

    void merge(TotalDur& other) { total += other.total; }
    std::uint64_t bytes() const { return sizeof(TotalDur); }
    std::string serialize() const { return std::to_string(total); }
    static TotalDur* deserialize(std::string_view b) {
        auto* s = new TotalDur();
        s->total = std::strtoull(std::string(b).c_str(), nullptr, 10);
        return s;
    }
    std::string finalize() { return std::to_string(total); }
};

struct RowCount {
    std::uint64_t rows = 0;

    explicit RowCount(const Config&) {}

    void step(const dftu_dataframe* df, Host) {
        rows += static_cast<std::uint64_t>(dftu_dataframe_num_rows(df));
    }
    void merge(RowCount& other) { rows += other.rows; }
    void finalize(Host h) {
        const std::string s = std::to_string(rows);
        h.emit_result("builder_plugin.rows", s);
    }
};

}  // namespace

extern "C" DFTU_PLUGIN_EXPORT dftu_plugin* dftracer_plugin(
    dftu_plugin_host* h, const dftu_value* config) {
    return plugin(h, config)
        .fold<RowCount>()
        .op(OP)
        .state<TotalDur>(STATE_NAME)
        .build();
}
