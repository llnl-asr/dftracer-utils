// SPDX-License-Identifier: MIT
// Exercises the host services a plugin calls synchronously: the DDSketch handle
// and the dftracer trace write/read round-trip. Drives them through a real
// PluginFold-wired dftu_plugin_host, the way a loaded plugin would.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dirent.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <plugins/test_abi_ctx.h>
#include <sys/stat.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>

using namespace dftracer::utils;

namespace {

struct TrivialSlice {
    explicit TrivialSlice(const dftracer::utils::plugins::Config&) {}
    void step(dataframe::FrameView, dftracer::utils::plugins::Host) {}
    void merge(TrivialSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

// Owns a PluginFold plus its trivial backing plugin, exposing the wired host.
// The fold references the plugin, so it must be destroyed before the plugin.
struct HostFixture {
    dstd::StringIntern intern;
    dftu_plugin* plugin =
        dftracer::utils::plugins::make_plugin<TrivialSlice>(nullptr);
    dstd::UniquePtr<plugins::host::PluginFold> fold =
        dstd::make_unique<plugins::host::PluginFold>(plugin, intern);
    dftu_plugin_host& host() { return fold->host(); }
    dftu_ctx* ctx() { return ctx_.get(); }
    test_plugin_common::Ctx ctx_;
    dftu_str intern_str(dstd::CStringView s) {
        return host().intern(host().h, s.c_str(),
                             static_cast<std::uint32_t>(s.size()));
    }
    ~HostFixture() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
};

std::uint64_t file_size(dstd::CStringView path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0
               ? static_cast<std::uint64_t>(st.st_size)
               : 0;
}

}  // namespace

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

    dstd::String trace = env.dir() + "/out.pfw.gz";
    dftu_trace_writer* w = nullptr;
    REQUIRE(tr->trace_open_write(fx.ctx(), host.h, trace.c_str(), &w) ==
            DFTU_OK);
    REQUIRE(w != nullptr);

    const std::uint32_t N = 50;
    dstd::Vector<dstd::String> cats(N, "POSIX"), names(N, "write");
    dstd::Vector<std::uint64_t> pid(N, 7), tid(N, 9), ts(N), dur(N, 3);
    dstd::Vector<std::int64_t> ph(N, DFTU_PH_COMPLETE);
    for (std::uint32_t i = 0; i < N; ++i) ts[i] = 1000000 + i * 100;

    auto string_col = [](dstd::Span<const dstd::String> vals) -> dftu_series* {
        dstd::Vector<std::int32_t> offsets(vals.size() + 1, 0);
        dstd::String data;
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

    CHECK(tr->trace_write(fx.ctx(), host.h, w, df) == DFTU_OK);
    dftu_dataframe_free(df);
    CHECK(tr->trace_close(fx.ctx(), host.h, w) == DFTU_OK);
    REQUIRE(file_size(trace) > 0);

    struct Sink {
        std::uint64_t count = 0;
        bool first_ok = false;
    };
    Sink sink{};
    auto on_batch = [](dftu_ctx*, const void* item, void* ud) -> dftu_status {
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
            std::int64_t len = 0;
            const char* p = dftu_series_string_at(name_col, 0, &len);
            const dstd::StringView nm =
                p ? dstd::StringView(p, static_cast<std::size_t>(len))
                  : dstd::StringView();
            s->first_ok = ts_data[0] == 1000000 && nm == "write";
        }
        s->count += static_cast<std::uint64_t>(n);
        if (ts_col) dftu_series_free(ts_col);
        if (name_col) dftu_series_free(name_col);
        return DFTU_OK;
    };

    int rr = tr->trace_read(fx.ctx(), host.h, trace.c_str(), on_batch, &sink);
    CHECK(rr == DFTU_OK);
    CHECK(sink.count == N);
    CHECK(sink.first_ok);
    MESSAGE("trace round-trip: wrote=" << N << " read=" << sink.count);
}

TEST_CASE(
    "plugin host: trace read keeps lines with a huge number, a bad escape or "
    "bad bytes") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    dftu_plugin_host& host = fx.host();
    const auto* tr = static_cast<const dftu_svc_trace*>(
        host.get_service(host.h, DFTU_SVC_TRACE));
    REQUIRE(tr != nullptr);

    const dstd::String pfw = env.dir() + "/refused.pfw";
    {
        dftu_utils_test::FileOut ofs(pfw);
        ofs << R"({"ph":"X","name":"a","cat":"P","pid":1,"tid":1,"ts":100,"dur":5,"args":{"x":1e400}})"
            << "\n"
            << R"({"ph":"X","name":"b\q","cat":"P","pid":1,"tid":1,"ts":110,"dur":5})"
            << "\n"
            << dstd::String(
                   "{\"ph\":\"X\",\"name\":\"c\xff"
                   "d\",\"cat\":\"P\",\"pid\":1,\"tid\":1,\"ts\":120,"
                   "\"dur\":5}")
            << "\n"
            << R"({"ph":"X","name":"d","cat":"P","pid":1,"tid":1,"ts":130,"dur":5,"args":{"n":123456789012345678901234567890}})"
            << "\n"
            << R"({"ph":"X","name":"e","cat":"P","pid":1,"tid":1,"ts":140,"dur":5})"
            << "\n";
    }
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();

    struct Sink {
        dstd::Vector<dstd::String> names;
    };
    Sink sink;
    auto on_batch = [](dftu_ctx*, const void* item, void* ud) -> dftu_status {
        const auto* batch_df = static_cast<const dftu_dataframe*>(item);
        auto* s = static_cast<Sink*>(ud);
        dftu_series* name_col = dftu_dataframe_column(batch_df, "name");
        const std::int64_t n = dftu_dataframe_num_rows(batch_df);
        for (std::int64_t i = 0; i < n; ++i) {
            std::int64_t len = 0;
            const char* p = dftu_series_string_at(name_col, i, &len);
            s->names.emplace_back(p, static_cast<std::size_t>(len));
        }
        if (name_col) dftu_series_free(name_col);
        return DFTU_OK;
    };
    REQUIRE(tr->trace_read(fx.ctx(), host.h, gz.c_str(), on_batch, &sink) ==
            DFTU_OK);
    std::sort(sink.names.begin(), sink.names.end());
    CHECK(sink.names ==
          dstd::Vector<dstd::String>{"a", R"(b\q)", "c?d", "d", "e"});
}

TEST_CASE("plugin host: trace close writes line-ended members and the tail") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    dftu_plugin_host& host = fx.host();
    const auto* tr = static_cast<const dftu_svc_trace*>(
        host.get_service(host.h, DFTU_SVC_TRACE));
    REQUIRE(tr != nullptr);

    const dstd::String dir = env.dir() + "/tw";
    ::mkdir(dir.c_str(), 0755);
    const dstd::String trace = dir + "/big.pfw.gz";
    dftu_trace_writer* w = nullptr;
    REQUIRE(tr->trace_open_write(fx.ctx(), host.h, trace.c_str(), &w) ==
            DFTU_OK);
    REQUIRE(w != nullptr);

    dstd::String expected;
    const dstd::String line(999, 'x');
    while (expected.size() < 2 * dftracer::utils::common::constants::indexer::
                                         DEFAULT_CHECKPOINT_SIZE +
                                 1024) {
        expected += line + "\n";
    }
    expected += "{\"tail\":true}";
    reinterpret_cast<plugins::host::PluginTraceWriter*>(w)->buffer = expected;
    REQUIRE(tr->trace_close(fx.ctx(), host.h, w) == DFTU_OK);

    dstd::String decoded, last;
    {
        dstd::String gz =
            dstd::fs::read_file(dstd::PathView(trace)).value_or(dstd::String());
        z_stream zs{};
        REQUIRE(inflateInit2(&zs, 15 + 16) == Z_OK);
        zs.next_in = reinterpret_cast<Bytef*>(gz.data());
        zs.avail_in = static_cast<uInt>(gz.size());
        dstd::Vector<char> buf(1 << 16);
        while (zs.avail_in > 0) {
            zs.next_out = reinterpret_cast<Bytef*>(buf.data());
            zs.avail_out = static_cast<uInt>(buf.size());
            const int rc = inflate(&zs, Z_NO_FLUSH);
            REQUIRE((rc == Z_OK || rc == Z_STREAM_END));
            decoded.append(buf.data(), buf.size() - zs.avail_out);
            if (rc == Z_STREAM_END) {
                last.push_back(decoded.back());
                REQUIRE(inflateReset(&zs) == Z_OK);
            }
        }
        inflateEnd(&zs);
    }
    CHECK(decoded == expected);
    CHECK(last.size() > 1);
    for (std::size_t i = 0; i + 1 < last.size(); ++i) CHECK(last[i] == '\n');

    std::size_t files = 0;
    for (auto* d = ::opendir(dir.c_str()); d;) {
        while (auto* e = ::readdir(d))
            if (e->d_name[0] != '.') ++files;
        ::closedir(d);
        break;
    }
    CHECK(files == 1);
}

TEST_CASE("plugin host: trace write prints a float arg with 17 digits") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    dftu_plugin_host& host = fx.host();
    const auto* tr = static_cast<const dftu_svc_trace*>(
        host.get_service(host.h, DFTU_SVC_TRACE));
    REQUIRE(tr != nullptr);

    dftu_trace_writer* w = nullptr;
    REQUIRE(tr->trace_open_write(fx.ctx(), host.h,
                                 (env.dir() + "/f.pfw.gz").c_str(),
                                 &w) == DFTU_OK);
    REQUIRE(w != nullptr);
    const double values[3] = {0.1, 100.0, 1e20};
    const char* col_names[1] = {"args.v"};
    dftu_series* cols[1] = {
        dftu_series_new_flat(DFTU_TYPE_FLOAT64, values, 3, nullptr)};
    dftu_dataframe* df = dftu_dataframe_new(col_names, cols, 1);
    REQUIRE(df != nullptr);
    CHECK(tr->trace_write(fx.ctx(), host.h, w, df) == DFTU_OK);
    dftu_dataframe_free(df);
    const dstd::String& buffer =
        reinterpret_cast<plugins::host::PluginTraceWriter*>(w)->buffer;
    CHECK(buffer.find("\"v\":0.10000000000000001}") != dstd::String::npos);
    CHECK(buffer.find("\"v\":100}") != dstd::String::npos);
    CHECK(buffer.find("\"v\":1e+20}") != dstd::String::npos);
    CHECK(tr->trace_close(fx.ctx(), host.h, w) == DFTU_OK);
}

TEST_CASE(
    "plugin host: a trace_read callback that returns an error fails the read") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    const dstd::String pfw = env.dir() + "/throw.pfw";
    {
        dftu_utils_test::FileOut ofs(pfw);
        ofs << R"({"ph":"X","name":"a","cat":"P","pid":1,"tid":1,"ts":100,"dur":5})"
            << "\n";
    }
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();

    dftracer::utils::plugins::Host h =
        dftracer::utils::plugins::detail::host_from_raw(&fx.host(), fx.ctx());
    int calls = 0;
    auto failed = h.trace_read(
        gz.c_str(), [&](const dftu_dataframe*) -> dstd::Result<void> {
            ++calls;
            return dftracer::utils::plugins::internal("boom");
        });
    REQUIRE_FALSE(failed.has_value());
    CHECK(dftracer::utils::plugins::status_of(failed.error()) < 0);
    CHECK(fx.ctx_.message().find("boom") != dstd::String::npos);
    CHECK(calls == 1);
    CHECK(h.trace_read(gz.c_str(), [&](const dftu_dataframe*) {}).has_value());
}
