// SPDX-License-Identifier: MIT
// Drives the C consumers (compose_abi_c_consumer.c, duql_abi_c_consumer.c):
// the host is built here in C++, but every ABI call happens in a C translation
// unit through the raw vtables, so this proves the plugin C ABIs work
// end-to-end from real C, not just from C++.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/fold_adapter.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/runtime/runtime.h>
// After fold_adapter.h so nanoarrow is set up before dataframe/abi.h's
// arrow_abi.
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <utilities/plugins/abi_c_consumers.h>

#include <cstdint>

using namespace dftracer::utils;

namespace {

struct TrivialSlice {
    explicit TrivialSlice(const dftracer::utils::plugins::Config&) {}
    void step(const dftu_dataframe*, dftracer::utils::plugins::Host) {}
    void merge(TrivialSlice&) {}
    void finalize(dftracer::utils::plugins::Host) {}
};

// Owning wrapper for a hand-built test dataframe (freed on scope exit).
struct TestFrame {
    dftu_dataframe* df = nullptr;
    ~TestFrame() {
        if (df) dftu_dataframe_free(df);
    }
    TestFrame(const TestFrame&) = delete;
    TestFrame& operator=(const TestFrame&) = delete;
    TestFrame() = default;
    TestFrame(TestFrame&& o) noexcept : df(o.df) { o.df = nullptr; }
};

dftu_series* string_column(const dstd::Vector<dstd::String>& vals) {
    dstd::Vector<std::int32_t> offsets(vals.size() + 1, 0);
    dstd::String data;
    for (std::size_t i = 0; i < vals.size(); ++i) {
        data += vals[i];
        offsets[i + 1] = static_cast<std::int32_t>(data.size());
    }
    return dftu_series_new_string(DFTU_TYPE_STRING, offsets.data(), data.data(),
                                  static_cast<std::int64_t>(vals.size()),
                                  nullptr);
}

// A [cat, name] string-column frame; only the columns dftu_svc_duql's tests
// need.
TestFrame cat_name_frame(const dstd::Vector<dstd::String>& cats,
                         const dstd::Vector<dstd::String>& names) {
    const char* col_names[2] = {"cat", "name"};
    dftu_series* cols[2] = {string_column(cats), string_column(names)};
    TestFrame f;
    f.df = dftu_dataframe_new(col_names, cols, 2);
    return f;
}

dftu_series* u64_column(const dstd::Vector<std::uint64_t>& vals) {
    return dftu_series_new_flat(DFTU_TYPE_UINT64, vals.data(),
                                static_cast<std::int64_t>(vals.size()),
                                nullptr);
}

dftu_series* i64_column(const dstd::Vector<std::int64_t>& vals) {
    return dftu_series_new_flat(DFTU_TYPE_INT64, vals.data(),
                                static_cast<std::int64_t>(vals.size()),
                                nullptr);
}

// A [cat, name, ph, pid, tid, ts, dur] frame - the columns trace_write reads
// (see fold_adapter.cpp's serialize_row) - N complete events on one pid/tid.
TestFrame trace_test_frame(std::uint32_t n) {
    dstd::Vector<dstd::String> cats(n, "POSIX"), names(n, "write");
    dstd::Vector<std::uint64_t> pid(n, 7), tid(n, 9), ts(n), dur(n, 3);
    dstd::Vector<std::int64_t> ph(n, DFTU_PH_COMPLETE);
    for (std::uint32_t i = 0; i < n; ++i) ts[i] = 1000000 + i * 100;
    const char* col_names[7] = {"cat", "name", "ph", "pid", "tid", "ts", "dur"};
    dftu_series* cols[7] = {string_column(cats), string_column(names),
                            i64_column(ph),      u64_column(pid),
                            u64_column(tid),     u64_column(ts),
                            u64_column(dur)};
    TestFrame f;
    f.df = dftu_dataframe_new(col_names, cols, 7);
    return f;
}

struct HostFixture {
    dstd::StringIntern intern;
    dftu_plugin* plugin;
    dstd::UniquePtr<plugins::PluginFold> fold;
    HostFixture()
        : plugin(dftracer::utils::plugins::make_plugin<TrivialSlice>(nullptr)),
          fold(dstd::make_unique<plugins::PluginFold>(plugin, intern)) {}
    ~HostFixture() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
    dftu_plugin_host& host() { return fold->host(); }
};

coro::CoroTask<void>* as_coro(::dftu_task* t) {
    return reinterpret_cast<coro::CoroTask<void>*>(t);
}

void drive(::dftu_task* t) {
    Runtime rt(1);
    rt.submit(coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                  co_await *as_coro(t);
              }),
              "c-abi")
        .wait();
    rt.shutdown();
}

}  // namespace

TEST_CASE("C ABI: dftu_svc_compose pipe threads a value from C") {
    HostFixture fx;
    std::int64_t in = 5, out = 0;
    int rc = -1;
    ::dftu_task* t = dftu_test_compose_pipe(&fx.host(), &in, &out, &rc);
    REQUIRE(t != nullptr);
    drive(t);
    CHECK(rc == 0);
    CHECK(out == 20);  // (5*2)+10
}

TEST_CASE("C ABI: dftu_svc_compose when_all concatenates from C") {
    HostFixture fx;
    std::int64_t in = 5;
    std::int64_t out[2] = {0, 0};
    int rc = -1;
    ::dftu_task* t = dftu_test_compose_all(&fx.host(), &in, out, &rc);
    REQUIRE(t != nullptr);
    drive(t);
    CHECK(out[0] == 10);  // 5*2
    CHECK(out[1] == 15);  // 5+10
}

TEST_CASE("C ABI: dftu_svc_compose type-checks a pipe from C") {
    HostFixture fx;
    CHECK(dftu_test_compose_typecheck(&fx.host()) == 1);
}

TEST_CASE("C ABI: dftu_svc_duql compile + match from C") {
    HostFixture fx;
    dftu_plugin_host& host = fx.host();

    dstd::String src = "cat == \"POSIX\"";
    auto len = static_cast<std::uint32_t>(src.size());

    // Row 0 is the hit (cat=POSIX), row 1 the miss (cat=STDIO); both name
    // "read".
    TestFrame frame = cat_name_frame({"POSIX", "STDIO"}, {"read", "read"});

    CHECK(dftu_test_query_match(&host, src.data(), len, frame.df, 0) == 1);
    CHECK(dftu_test_query_match(&host, src.data(), len, frame.df, 1) == 0);

    dstd::String bad = "cat ==";
    CHECK(dftu_test_query_match(&host, bad.data(),
                                static_cast<std::uint32_t>(bad.size()),
                                frame.df, 0) == -1);  // malformed -> compile
                                                      // failure
    CHECK(dftu_test_query_null_is_safe(&host, frame.df, 0) ==
          0);                                         // null query is safe
}

TEST_CASE("C ABI: dftu_svc_sketch add/result/merge from C") {
    HostFixture fx;
    double p50 = 0;
    std::uint64_t count = 0;
    REQUIRE(dftu_test_sketch(&fx.host(), &p50, &count) == 0);
    CHECK(count == 100);
    CHECK(p50 == doctest::Approx(50.0).epsilon(0.1));  // median of 1..100

    std::uint64_t merged = 0;
    REQUIRE(dftu_test_sketch_merge(&fx.host(), &merged) == 0);
    CHECK(merged == 100);  // 50 + 50
}

TEST_CASE("C ABI: dftu_svc_ops runs a host utility op from C") {
    HostFixture fx;
    std::uint64_t hash = 0;
    dstd::String data = "hello-world";
    REQUIRE(dftu_test_ops_fnv1a(&fx.host(), data.data(),
                                static_cast<std::uint32_t>(data.size()),
                                &hash) == 0);
    CHECK(hash != 0);
    // Deterministic: the same input hashes the same.
    std::uint64_t again = 0;
    REQUIRE(dftu_test_ops_fnv1a(&fx.host(), data.data(),
                                static_cast<std::uint32_t>(data.size()),
                                &again) == 0);
    CHECK(hash == again);
    CHECK(dftu_test_ops_find(&fx.host()) == 1);
}

TEST_CASE("C ABI: dftu_svc_trace write then read round-trips from C") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    dftu_plugin_host& host = fx.host();

    const std::uint32_t N = 20;
    TestFrame frame = trace_test_frame(N);

    dstd::String path = env.dir() + "/c_abi.pfw.gz";
    int read = 0;
    REQUIRE(dftu_test_trace_roundtrip(&host, path.c_str(), frame.df, &read) ==
            0);
    CHECK(read == static_cast<int>(N));
}

TEST_CASE("C ABI: dftu_io write/read round-trips from C") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    dftu_plugin_host& host = fx.host();
    dstd::String path = env.dir() + "/io_c.bin";
    dstd::String data = "hello-io";
    char buf[16] = {0};
    int fd = -1;
    std::int64_t wn = 0, rn = 0;
    int crc = -1;

    // C has no coroutines, so the harness sequences the C-created tasks: the fd
    // from open flows into write/pread/close.
    Runtime rt(1);
    rt.submit(
          coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
              co_await *as_coro(dftu_test_io_open(&host, path.c_str(), &fd));
              if (fd >= 0) {
                  co_await *as_coro(dftu_test_io_write(
                      &host, fd, data.data(),
                      static_cast<std::uint64_t>(data.size()), &wn));
                  co_await *as_coro(dftu_test_io_pread(
                      &host, fd, buf, static_cast<std::uint64_t>(data.size()),
                      0, &rn));
                  co_await *as_coro(dftu_test_io_close(&host, fd, &crc));
              }
          }),
          "io")
        .wait();
    rt.shutdown();

    REQUIRE(fd >= 0);
    CHECK(wn == static_cast<std::int64_t>(data.size()));
    CHECK(rn == static_cast<std::int64_t>(data.size()));
    CHECK(dstd::String(buf, data.size()) == data);
    CHECK(crc == 0);
}

TEST_CASE("C ABI: dftu_svc_writer create/open/chunk/close from C") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    dftu_plugin_host& host = fx.host();
    dstd::String path = env.dir() + "/writer_c.out";
    dstd::String data = "chunk-from-c";

    ::dftu_writer* w = dftu_test_writer_create(&host, path.c_str());
    REQUIRE(w != nullptr);
    Runtime rt(1);
    rt.submit(coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                  co_await *as_coro(dftu_test_writer_open(&host, w));
                  co_await *as_coro(dftu_test_writer_chunk(
                      &host, w, data.data(),
                      static_cast<std::uint64_t>(data.size())));
                  co_await *as_coro(dftu_test_writer_close(&host, w));
              }),
              "writer")
        .wait();
    rt.shutdown();

    // The sharded writer emits one file per worker: `<path>.shard_0`.
    const auto sz = dstd::fs::file_size(path + ".shard_0");
    CHECK(sz.has_value());
    CHECK(sz.value_or(0) > 0);
}
