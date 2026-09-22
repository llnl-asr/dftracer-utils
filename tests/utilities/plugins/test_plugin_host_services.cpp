// Exercises the host services a plugin calls synchronously: Arrow batch export,
// Arrow IPC write, the DDSketch handle, and the dftracer trace write/read
// round-trip. Drives them through a real PluginFold-wired dftu_plugin_host, the
// way a loaded plugin would.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/plugins/abi.h>
// fold_adapter.h transitively pulls nanoarrow (ArrowArray/ArrowSchema); it must
// precede plugin.h so arrow_abi.h no-ops rather than tripping nanoarrow's outer
// ARROW_FLAG_DICTIONARY_ORDERED sentinel.
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/stat.h>

// dftu_svc_arrow::batch_to_arrow was removed with the row ABI; the dataframe
// engine's own Arrow bridge (DataFrame::to_arrow) now builds the test arrays
// arrow_write_ipc round-trips.
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <dftracer/utils/dataframe/arrow.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing_utilities.h"

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::StringIntern;
using dftracer::utils::plugins::PluginFold;
namespace coro = dftracer::utils::coro;

namespace {

struct TrivialSlice {
    explicit TrivialSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftu_dataframe*, dftracer::utils::plugins::Host) {}
    void merge(TrivialSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

// Owns a PluginFold plus its trivial backing plugin, exposing the wired host.
// The fold references the plugin, so it must be destroyed before the plugin.
struct HostFixture {
    StringIntern intern;
    dftu_plugin* plugin =
        dftracer::utils::plugins::make_plugin<TrivialSlice>(nullptr);
    std::unique_ptr<PluginFold> fold =
        std::make_unique<PluginFold>(plugin, intern);
    dftu_plugin_host& host() { return fold->host(); }
    dftu_str intern_str(const char* s) {
        return host().intern(host().h, s,
                             static_cast<std::uint32_t>(strlen(s)));
    }
    ~HostFixture() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
};

std::uint64_t file_size(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0
               ? static_cast<std::uint64_t>(st.st_size)
               : 0;
}

namespace dfti = dftracer::utils::trace::internal;

}  // namespace

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
TEST_CASE("plugin host: arrow_write_ipc writes a non-empty IPC file") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    dftu_plugin_host& host = fx.host();

    namespace dfd = dftracer::utils::dataframe;
    const std::uint64_t ts0[3] = {500, 501, 502};
    dfd::DataFrame df;
    df.names = {"cat", "name", "ts"};
    df.columns.push_back(dfd::Series::strings({"STDIO", "STDIO", "STDIO"}));
    df.columns.push_back(dfd::Series::strings({"fwrite", "fwrite", "fwrite"}));
    df.columns.push_back(dfd::Series::flat(dfd::TypeId::Uint64, ts0, 3));
    dfd::OwnedArrow owned = df.to_arrow();
    REQUIRE(owned);

    const auto* arrow = static_cast<const dftu_svc_arrow*>(
        host.get_service(host.h, DFTU_SVC_ARROW));
    REQUIRE(arrow != nullptr);

    std::string ipc = env.get_dir() + "/x.arrow";
    int wr = arrow->arrow_write_ipc(host.h, owned.array(), owned.schema(),
                                    ipc.c_str());
    CHECK(wr == 0);
    CHECK(file_size(ipc) > 0);
    MESSAGE("arrow ipc: " << ipc << " size=" << file_size(ipc));

    owned.reset();

    ArrowArray back{};
    ArrowSchema back_sch{};
    int rd = arrow->arrow_read_ipc(host.h, ipc.c_str(), &back, &back_sch);
    CHECK(rd == 0);
    REQUIRE(back.release != nullptr);
    REQUIRE(back_sch.release != nullptr);
    CHECK(back.length == 3);  // rows survive the write/read round-trip
    REQUIRE(back.n_children == 3);
    const ArrowArray* ts_col = back.children[2];
    const std::uint64_t* ts =
        static_cast<const std::uint64_t*>(ts_col->buffers[1]);
    CHECK(ts[0] == 500);
    CHECK(ts[2] == 502);
    MESSAGE("arrow ipc read: rows=" << back.length << " ts0=" << ts[0]);
    back.release(&back);
    back_sch.release(&back_sch);
}
#endif

TEST_CASE("plugin host: DDSketch handle add/merge/result round-trips") {
    HostFixture fx;
    dftu_plugin_host& host = fx.host();

    const auto* sk = static_cast<const dftu_svc_sketch*>(
        host.get_service(host.h, DFTU_SVC_SKETCH));
    REQUIRE(sk != nullptr);

    dftu_sketch* a = sk->sketch_create(host.h);
    dftu_sketch* b = sk->sketch_create(host.h);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    for (int v = 1; v <= 500; ++v)
        sk->sketch_add(host.h, a, static_cast<double>(v), 1.0);
    for (int v = 501; v <= 1000; ++v)
        sk->sketch_add(host.h, b, static_cast<double>(v), 1.0);
    sk->sketch_merge(host.h, a, b);

    dftu_quantiles q = sk->sketch_result(host.h, a);
    CHECK(q.count == 1000);
    CHECK(q.min == doctest::Approx(1.0));
    CHECK(q.max == doctest::Approx(1000.0));
    CHECK(q.mean == doctest::Approx(500.5).epsilon(0.01));

    // DDSketch's default 1% relative accuracy bounds the returned quantile to
    // [x*(1-a), x*(1+a)]; allow a slightly wider band for bucket rounding.
    const double p50_band = 500.0 * 0.02;
    CHECK(q.p50 >= 500.0 - p50_band);
    CHECK(q.p50 <= 500.0 + p50_band);
    MESSAGE("sketch handle: count=" << q.count << " min=" << q.min
                                    << " max=" << q.max << " p50=" << q.p50
                                    << " p90=" << q.p90 << " p99=" << q.p99);

    sk->sketch_free(host.h, a);
    sk->sketch_free(host.h, b);
}

TEST_CASE("plugin host: trace write then read round-trips events") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    dftu_plugin_host& host = fx.host();

    const auto* tr = static_cast<const dftu_svc_trace*>(
        host.get_service(host.h, DFTU_SVC_TRACE));
    REQUIRE(tr != nullptr);

    std::string trace = env.get_dir() + "/out.pfw.gz";
    dftu_trace_writer* w = tr->trace_open_write(host.h, trace.c_str());
    REQUIRE(w != nullptr);

    const std::uint32_t N = 50;
    std::vector<std::string> cats(N, "POSIX"), names(N, "write");
    std::vector<std::uint64_t> pid(N, 7), tid(N, 9), ts(N), dur(N, 3);
    std::vector<std::int64_t> ph(N, DFTU_PH_COMPLETE);
    for (std::uint32_t i = 0; i < N; ++i) ts[i] = 1000000 + i * 100;

    auto string_col = [](const std::vector<std::string>& vals) -> dftu_series* {
        std::vector<std::int32_t> offsets(vals.size() + 1, 0);
        std::string data;
        for (std::size_t i = 0; i < vals.size(); ++i) {
            data += vals[i];
            offsets[i + 1] = static_cast<std::int32_t>(data.size());
        }
        return dftu_series_new_string(
            DFTU_TYPE_STRING, offsets.data(), data.data(),
            static_cast<std::int64_t>(vals.size()), nullptr);
    };
    const char* col_names[7] = {"cat", "name", "ph", "pid", "tid", "ts", "dur"};
    dftu_series* cols[7] = {
        string_col(cats),
        string_col(names),
        dftu_series_new_flat(DFTU_TYPE_INT64, ph.data(),
                             static_cast<std::int64_t>(N), nullptr),
        dftu_series_new_flat(DFTU_TYPE_UINT64, pid.data(),
                             static_cast<std::int64_t>(N), nullptr),
        dftu_series_new_flat(DFTU_TYPE_UINT64, tid.data(),
                             static_cast<std::int64_t>(N), nullptr),
        dftu_series_new_flat(DFTU_TYPE_UINT64, ts.data(),
                             static_cast<std::int64_t>(N), nullptr),
        dftu_series_new_flat(DFTU_TYPE_UINT64, dur.data(),
                             static_cast<std::int64_t>(N), nullptr)};
    dftu_dataframe* df = dftu_dataframe_new(col_names, cols, 7);
    REQUIRE(df != nullptr);

    CHECK(tr->trace_write(host.h, w, df) == 0);
    dftu_dataframe_free(df);
    CHECK(tr->trace_close(host.h, w) == 0);
    REQUIRE(file_size(trace) > 0);

    struct Sink {
        std::uint64_t count = 0;
        bool first_ok = false;
    };
    Sink sink{};
    auto on_batch = [](const void* item, void* ud) {
        const auto* batch_df = static_cast<const dftu_dataframe*>(item);
        auto* s = static_cast<Sink*>(ud);
        dftu_series* ts_col = dftu_dataframe_column(batch_df, "ts");
        dftu_series* name_col = dftu_dataframe_column(batch_df, "name");
        const std::int64_t n = dftu_dataframe_num_rows(batch_df);
        if (s->count == 0 && ts_col && name_col && n > 0 &&
            dftu_series_type(ts_col) == DFTU_TYPE_UINT64 &&
            dftu_series_type(name_col) == DFTU_TYPE_STRING) {
            const auto* ts_data =
                static_cast<const std::uint64_t*>(dftu_series_data(ts_col));
            const auto* off = dftu_series_offsets(name_col);
            const char* base =
                static_cast<const char*>(dftu_series_data(name_col));
            std::string_view nm(base + off[0],
                                static_cast<std::size_t>(off[1] - off[0]));
            s->first_ok = ts_data[0] == 1000000 && nm == "write";
        }
        s->count += static_cast<std::uint64_t>(n);
        if (ts_col) dftu_series_free(ts_col);
        if (name_col) dftu_series_free(name_col);
    };

    int rr = tr->trace_read(host.h, trace.c_str(), on_batch, &sink);
    CHECK(rr == 0);
    CHECK(sink.count == N);
    CHECK(sink.first_ok);
    MESSAGE("trace round-trip: wrote=" << N << " read=" << sink.count);
}
