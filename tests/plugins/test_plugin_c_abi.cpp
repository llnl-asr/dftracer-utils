// SPDX-License-Identifier: MIT
// Drives the C consumers (task_abi_c_consumer.c, duql_abi_c_consumer.c):
// the host is built here in C++, but every ABI call happens in a C translation
// unit through the raw vtables, so this proves the plugin C ABIs work
// end-to-end from real C, not just from C++.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/task_abi.h>
#include <dftracer/utils/runtime/runtime.h>
// After fold_adapter.h so nanoarrow is set up before dataframe/abi.h's
// arrow_abi.
#include <dirent.h>
#include <doctest/doctest.h>
#include <plugins/abi_c_consumers.h>
#include <plugins/test_plugin_common.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

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
    dstd::UniquePtr<plugins::host::PluginFold> fold;
    HostFixture()
        : plugin(dftracer::utils::plugins::make_plugin<TrivialSlice>(nullptr)),
          fold(dstd::make_unique<plugins::host::PluginFold>(plugin, intern)) {}
    ~HostFixture() {
        fold.reset();
        if (plugin && plugin->destroy) plugin->destroy(plugin->self);
    }
    dftu_plugin_host& host() { return fold->host(); }
};

// A ctx for calling the plugin ABI from outside a callback; it keeps the
// message a service reports.
struct Ctx {
    plugins::AbiContext ctx;
    dftu_ctx* get() { return ctx.get(); }
    dstd::String message() { return dstd::String(ctx.message()); }
};

dftu_status run(::dftu_task* t, dstd::String* message = nullptr) {
    return test_plugin_common::run_task(t, message);
}

Ctx* g_harness_ctx = nullptr;

void copy_text(const dstd::String& text, char* out, std::uint64_t cap) {
    std::snprintf(out, static_cast<std::size_t>(cap), "%s", text.c_str());
}

// The callbacks the C scenarios use to run a task, record a check and read the
// message of a refused call.
dftu_test_harness make_harness(Ctx& c) {
    g_harness_ctx = &c;
    dftu_test_harness hn{};
    hn.run = [](::dftu_task* t, char* message, std::uint64_t cap) {
        dstd::String text;
        const dftu_status st = run(t, &text);
        copy_text(text, message, cap);
        return st;
    };
    hn.expect = [](const char* what, int ok) {
        CHECK_MESSAGE(ok, doctest::String(what));
    };
    hn.ctx_message = [](::dftu_ctx*, char* message, std::uint64_t cap) {
        copy_text(g_harness_ctx->message(), message, cap);
    };
    return hn;
}

}  // namespace

TEST_CASE("C ABI: dftu_svc_task pipe threads a value from C") {
    HostFixture fx;
    Ctx c;
    std::int64_t in = 5, out = 0;
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_task_pipe(c.get(), &fx.host(), &in, &out, &t) >= 0);
    REQUIRE(t != nullptr);
    CHECK(run(t) == DFTU_OK);
    CHECK(out == 20);  // (5*2)+10
}

TEST_CASE("C ABI: dftu_svc_task all concatenates from C") {
    HostFixture fx;
    Ctx c;
    std::int64_t in = 5;
    std::int64_t out[2] = {0, 0};
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_task_all(c.get(), &fx.host(), &in, out, &t) >= 0);
    CHECK(run(t) == DFTU_OK);
    CHECK(out[0] == 10);  // 5*2
    CHECK(out[1] == 15);  // 5+10
}

TEST_CASE("C ABI: dftu_svc_task type-checks a pipe from C") {
    HostFixture fx;
    Ctx c;
    dftu_status mismatch = DFTU_OK;
    CHECK(dftu_test_task_typecheck(c.get(), &fx.host(), &mismatch) == DFTU_OK);
    CHECK(mismatch == DFTU_E_INVALID);
    CHECK(c.message().find("not the input") != dstd::String::npos);
}

TEST_CASE("C ABI: all cancels the rest at the first error") {
    HostFixture fx;
    Ctx c;
    std::int64_t in = 1;
    std::int64_t out[2] = {0, 0};
    int saw_cancel = 0;
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_task_all_error(c.get(), &fx.host(), &in, out, &saw_cancel,
                                     &t) >= 0);
    dstd::String message;
    // The failing job's own status and message decide; the sibling saw the
    // cancel through dftu_check before the task finished.
    CHECK(run(t, &message) == DFTU_E_PARSE);
    CHECK(message.find("c_fail: refused on purpose") != dstd::String::npos);
    CHECK(saw_cancel == 1);
}

TEST_CASE("C ABI: any is decided by the first job to finish") {
    HostFixture fx;
    Ctx c;
    std::int64_t in = 5, out = 0;
    int saw_cancel = 0;
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_task_any_first(c.get(), &fx.host(), &in, &out,
                                     &saw_cancel, &t) >= 0);
    CHECK(run(t) == DFTU_OK);
    CHECK(out == 15);
    CHECK(saw_cancel == 1);  // the slow job was cancelled, not left running
}

TEST_CASE("C ABI: any against a sleep job is a deadline") {
    HostFixture fx;
    Ctx c;
    int saw_cancel = 0;
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_task_any_deadline(c.get(), &fx.host(), &saw_cancel, &t) >=
            0);
    CHECK(run(t) == DFTU_E_TIMEOUT);
    CHECK(saw_cancel == 1);
}

TEST_CASE("C ABI: map keeps results in order within max_parallel") {
    HostFixture fx;
    Ctx c;
    constexpr std::uint64_t N = 16;
    std::int64_t items[N], results[N];
    for (std::uint64_t i = 0; i < N; ++i) {
        items[i] = static_cast<std::int64_t>(i);
        results[i] = -1;
    }
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_task_map(c.get(), &fx.host(), items, results, N, 2, -1,
                               nullptr, &t) >= 0);
    CHECK(run(t) == DFTU_OK);
    for (std::uint64_t i = 0; i < N; ++i)
        CHECK(results[i] == static_cast<std::int64_t>(i) * 2);
    CHECK(dftu_test_task_map_max_seen() <= 2);
    CHECK(dftu_test_task_map_max_seen() >= 1);
}

TEST_CASE("C ABI: map fails with the first error") {
    HostFixture fx;
    Ctx c;
    constexpr std::uint64_t N = 16;
    std::int64_t items[N], results[N];
    for (std::uint64_t i = 0; i < N; ++i) {
        items[i] = static_cast<std::int64_t>(i);
        results[i] = -1;
    }
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_task_map(c.get(), &fx.host(), items, results, N, 2, 3,
                               nullptr, &t) >= 0);
    dstd::String message;
    CHECK(run(t, &message) == DFTU_E_INVALID);
    CHECK(message.find("item refused") != dstd::String::npos);
}

TEST_CASE("C ABI: spawn runs a function and reports its status") {
    HostFixture fx;
    Ctx c;
    for (dftu_job_where where : {DFTU_JOB_CPU, DFTU_JOB_BLOCKING}) {
        dftu_spawn_probe ok{0, DFTU_OK};
        ::dftu_task* t = nullptr;
        REQUIRE(dftu_test_task_spawn(c.get(), &fx.host(), &ok, where, &t) >= 0);
        CHECK(run(t) == DFTU_OK);
        CHECK(ok.ran == 1);

        dftu_spawn_probe bad{0, DFTU_E_IO};
        REQUIRE(dftu_test_task_spawn(c.get(), &fx.host(), &bad, where, &t) >=
                0);
        dstd::String message;
        CHECK(run(t, &message) == DFTU_E_IO);
        CHECK(message == "spawned failed");
        CHECK(bad.ran == 1);
    }
}

TEST_CASE("C ABI: drive runs rounds and status reads a failed task") {
    HostFixture fx;
    Ctx c;
    {
        dftu_drive_report rep{};
        ::dftu_task* t = nullptr;
        REQUIRE(dftu_test_task_drive(c.get(), &fx.host(), &rep, &t) >= 0);
        CHECK(run(t) == DFTU_OK);
        CHECK(rep.steps == 3);  // start, start, read
        CHECK(rep.first_status == DFTU_OK);
        CHECK(rep.second_status == DFTU_E_IO);
        CHECK(rep.released == 1);
    }
    {
        // Returning the failed task's status fails the drive with its message.
        dftu_drive_report rep{};
        rep.propagate = 1;
        ::dftu_task* t = nullptr;
        REQUIRE(dftu_test_task_drive(c.get(), &fx.host(), &rep, &t) >= 0);
        dstd::String message;
        CHECK(run(t, &message) == DFTU_E_IO);
        CHECK(message == "spawned failed");
        CHECK(rep.released == 1);
    }
    {
        // A step with nothing to wait for fails the drive, which then ends
        // without another step and still releases the state.
        dftu_drive_report rep{};
        rep.empty_round = 1;
        ::dftu_task* t = nullptr;
        REQUIRE(dftu_test_task_drive(c.get(), &fx.host(), &rep, &t) >= 0);
        CHECK(run(t) == DFTU_E_INVALID);
        CHECK(rep.steps == 1);
        CHECK(rep.released == 1);
    }
    {
        // A drive task freed before it ran still releases the state.
        dftu_drive_report rep{};
        ::dftu_task* t = nullptr;
        REQUIRE(dftu_test_task_drive(c.get(), &fx.host(), &rep, &t) >= 0);
        plugins::free_abi_task(t);
        CHECK(rep.steps == 0);
        CHECK(rep.released == 1);
    }
}

namespace {

coro::CoroTask<void> end_body(plugins::AbiTask* task) {
    task->status = DFTU_END;
    co_return;
}

coro::CoroTask<void> refuse_body(plugins::AbiTask* task) {
    task->fail(DFTU_E_PARSE, "run refused on purpose");
    co_return;
}

}  // namespace

TEST_CASE(
    "C ABI: dftu_svc_task run blocks, frees the task and gives its status") {
    HostFixture fx;
    Ctx c;
    const auto* svc = static_cast<const dftu_svc_task*>(
        fx.host().get_service(fx.host().h, DFTU_SVC_TASK));
    REQUIRE(svc != nullptr);
    REQUIRE(svc->run != nullptr);

    dftu_spawn_probe ok{0, DFTU_OK};
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_task_spawn(c.get(), &fx.host(), &ok, DFTU_JOB_CPU, &t) >=
            0);
    CHECK(svc->run(c.get(), fx.host().h, t) == DFTU_OK);
    CHECK(ok.ran == 1);

    dftu_spawn_probe bad{0, DFTU_E_IO};
    REQUIRE(dftu_test_task_spawn(c.get(), &fx.host(), &bad, DFTU_JOB_CPU, &t) >=
            0);
    CHECK(svc->run(c.get(), fx.host().h, t) == DFTU_E_IO);
    CHECK(c.message() == "spawned failed");
    CHECK(bad.ran == 1);

    t = plugins::make_abi_task(
        [](plugins::AbiTask* task) { return refuse_body(task); });
    CHECK(svc->run(c.get(), fx.host().h, t) == DFTU_E_PARSE);
    CHECK(c.message() == "run refused on purpose");

    t = plugins::make_abi_task(
        [](plugins::AbiTask* task) { return end_body(task); });
    CHECK(svc->run(c.get(), fx.host().h, t) == DFTU_END);

    CHECK(svc->run(c.get(), fx.host().h, nullptr) == DFTU_E_INVALID);
    CHECK(svc->run(nullptr, fx.host().h, nullptr) == DFTU_E_INVALID);
}

TEST_CASE("C ABI: dftu_svc_duql compile + match from C") {
    HostFixture fx;
    dftu_plugin_host& host = fx.host();

    dstd::String src = "cat == \"POSIX\"";
    auto len = static_cast<std::uint32_t>(src.size());

    // Row 0 is the hit (cat=POSIX), row 1 the miss (cat=STDIO); both name
    // "read".
    TestFrame frame = cat_name_frame({"POSIX", "STDIO"}, {"read", "read"});

    Ctx c;
    CHECK(dftu_test_query_match(c.get(), &host, src.data(), len, frame.df, 0) ==
          1);
    CHECK(dftu_test_query_match(c.get(), &host, src.data(), len, frame.df, 1) ==
          0);

    dstd::String bad = "cat ==";
    CHECK(dftu_test_query_match(c.get(), &host, bad.data(),
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
    Ctx c;
    REQUIRE(dftu_test_ops_fnv1a(c.get(), &fx.host(), data.data(),
                                static_cast<std::uint32_t>(data.size()),
                                &hash) == 0);
    CHECK(hash != 0);
    // Deterministic: the same input hashes the same.
    std::uint64_t again = 0;
    REQUIRE(dftu_test_ops_fnv1a(c.get(), &fx.host(), data.data(),
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
    Ctx c;
    REQUIRE(dftu_test_trace_roundtrip(c.get(), &host, path.c_str(), frame.df,
                                      &read) == 0);
    CHECK(read == static_cast<int>(N));
}

TEST_CASE("C ABI: dftu_io write/read round-trips from C") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    dstd::String path = env.dir() + "/io_c.bin";
    dstd::String data = "hello-io";
    char buf[16] = {0};
    int fd = -1;
    std::int64_t wn = 0, rn = 0;
    ::dftu_task* t = nullptr;

    // C has no coroutines, so the harness runs the C-created tasks one after
    // the other: the fd from open flows into write/pread/close.
    REQUIRE(dftu_test_io_open(c.get(), &host, path.c_str(), &fd, &t) >= 0);
    REQUIRE(run(t) == DFTU_OK);
    REQUIRE(fd >= 0);
    REQUIRE(dftu_test_io_write(c.get(), &host, fd, data.data(),
                               static_cast<std::uint64_t>(data.size()), &wn,
                               &t) >= 0);
    CHECK(run(t) == DFTU_OK);
    REQUIRE(dftu_test_io_pread(c.get(), &host, fd, buf,
                               static_cast<std::uint64_t>(data.size()), 0, &rn,
                               &t) >= 0);
    CHECK(run(t) == DFTU_OK);
    REQUIRE(dftu_test_io_close(c.get(), &host, fd, &t) >= 0);
    CHECK(run(t) == DFTU_OK);

    CHECK(wn == static_cast<std::int64_t>(data.size()));
    CHECK(rn == static_cast<std::int64_t>(data.size()));
    CHECK(dstd::String(buf, data.size()) == data);
}

TEST_CASE("C ABI: dftu_io open of a missing file fails with NOT_FOUND") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dstd::String path = env.dir() + "/does_not_exist.bin";
    int fd = 123;
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_io_open_existing(c.get(), &fx.host(), path.c_str(), &fd,
                                       &t) >= 0);
    dstd::String message;
    CHECK(run(t, &message) == DFTU_E_NOT_FOUND);
    CHECK(message.find("open") != dstd::String::npos);
    CHECK(message.find("errno") != dstd::String::npos);
}

TEST_CASE("C ABI: dftu_io file system ops succeed and fail from C") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    const dftu_test_harness hn = make_harness(c);
    dftu_test_io_fs_ops(c.get(), &fx.host(), &hn, env.dir().c_str());
}

TEST_CASE("C ABI: dftu_io socket ops succeed and fail from C") {
    HostFixture fx;
    Ctx c;
    const dftu_test_harness hn = make_harness(c);
    dftu_test_io_socket_ops(c.get(), &fx.host(), &hn);
}

TEST_CASE("C ABI: dftu_io Linux-only ops work on Linux and refuse elsewhere") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    const dftu_test_harness hn = make_harness(c);
    dftu_test_io_linux_ops(c.get(), &fx.host(), &hn, env.dir().c_str());
}

TEST_CASE("C ABI: dftu_svc_fs stat_batch refuses paths of two file systems") {
    HostFixture fx;
    Ctx c;
    const auto* fs = static_cast<const dftu_svc_fs*>(
        fx.host().get_service(fx.host().h, DFTU_SVC_FS));
    REQUIRE(fs != nullptr);
    const char* paths[] = {"/tmp/local.txt", "s3://bucket/key.txt"};
    dftu_fs_stat out[2] = {};
    dftu_status status[2] = {};
    ::dftu_task* t = nullptr;
    CHECK(fs->stat_batch(c.get(), fx.host().h, paths, 2, out, status, &t) ==
          DFTU_E_INVALID);
    CHECK(t == nullptr);
    CHECK(c.message().find("one file system") != dstd::String::npos);
}

TEST_CASE("C ABI: dftu_svc_fs calls succeed and fail from C") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    const dftu_test_harness hn = make_harness(c);
    dftu_test_fs_ops(c.get(), &fx.host(), &hn, env.dir().c_str());
}

TEST_CASE("C ABI: dftu_svc_fileio writer create/open/chunk/close from C") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    dstd::String path = env.dir() + "/writer_c.out";
    dstd::String data = "chunk-from-c";

    ::dftu_writer* w = nullptr;
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_writer_create(c.get(), &host, path.c_str(), &w) >= 0);
    REQUIRE(w != nullptr);
    REQUIRE(dftu_test_writer_open(c.get(), &host, w, &t) >= 0);
    CHECK(run(t) == DFTU_OK);
    REQUIRE(dftu_test_writer_chunk(c.get(), &host, w, data.data(),
                                   static_cast<std::uint64_t>(data.size()),
                                   &t) >= 0);
    CHECK(run(t) == DFTU_OK);
    REQUIRE(dftu_test_writer_close(c.get(), &host, w, &t) >= 0);
    CHECK(run(t) == DFTU_OK);

    // The sharded writer emits one file per worker: `<path>.shard_0`.
    const auto sz = dstd::fs::file_size(path + ".shard_0");
    CHECK(sz.has_value());
    CHECK(sz.value_or(0) > 0);
}

namespace {

dstd::String write_text(const dstd::String& path, const dstd::String& text) {
    dstd::fs::write_file(dstd::PathView(path), text).discard();
    return path;
}

// Reads every remaining line of `r` through lines_next, `max` per batch.
dstd::Vector<dstd::String> read_lines(Ctx& c, dftu_plugin_host& host,
                                      ::dftu_line_reader* r, std::uint32_t max,
                                      dstd::Vector<std::uint64_t>* firsts) {
    dstd::Vector<dstd::String> lines;
    for (;;) {
        dftu_line_batch batch{};
        ::dftu_task* t = nullptr;
        REQUIRE(dftu_test_lines_next(c.get(), &host, r, max, &batch, &t) >= 0);
        const dftu_status status = run(t);
        if (status == DFTU_END) {
            CHECK(batch.count == 0);
            break;
        }
        REQUIRE(status == DFTU_OK);
        REQUIRE(batch.count > 0);
        if (firsts != nullptr) firsts->push_back(batch.first_line);
        for (std::uint32_t i = 0; i < batch.count; ++i) {
            CHECK(batch.lines[i].free_fn == nullptr);
            lines.push_back(
                dstd::String(static_cast<const char*>(batch.lines[i].data),
                             static_cast<std::size_t>(batch.lines[i].len)));
        }
    }
    return lines;
}

// How many entries of `dir` start with `prefix`.
int entries_with_prefix(const dstd::String& dir, const char* prefix) {
    int count = 0;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return 0;
    while (const struct dirent* e = ::readdir(d)) {
        if (std::strncmp(e->d_name, prefix, std::strlen(prefix)) == 0) ++count;
    }
    ::closedir(d);
    return count;
}

// A gzip file of `text` written through gz_open/gz_append/gz_close, in members
// of `member_size` bytes.
void write_gz_from_c(Ctx& c, dftu_plugin_host& host, const dstd::String& path,
                     const dstd::String& text, std::uint64_t member_size,
                     dftu_gz_summary* summary) {
    ::dftu_gz_writer* w = nullptr;
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_gz_open(c.get(), &host, path.c_str(), member_size, &w,
                              &t) >= 0);
    REQUIRE(w != nullptr);
    REQUIRE(run(t) == DFTU_OK);
    REQUIRE(dftu_test_gz_append(c.get(), &host, w, text.data(),
                                static_cast<std::uint64_t>(text.size())) >= 0);
    REQUIRE(dftu_test_gz_close(c.get(), &host, w, summary, &t) >= 0);
    REQUIRE(run(t) == DFTU_OK);
}

}  // namespace

TEST_CASE("C ABI: lines_open reads a plain file by line range from C") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    const dstd::String path =
        write_text(env.dir() + "/lines.txt", "one\ntwo\nthree\nfour\nfive\n");

    ::dftu_line_reader* r = nullptr;
    REQUIRE(dftu_test_lines_open(c.get(), &host, path.c_str(), 2, 4, &r) >= 0);
    dstd::Vector<std::uint64_t> firsts;
    auto lines = read_lines(c, host, r, 2, &firsts);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "two");
    CHECK(lines[2] == "four");
    REQUIRE(firsts.size() == 2);
    CHECK(firsts[0] == 2);
    CHECK(firsts[1] == 4);
    dftu_test_lines_close(&host, r);
}

TEST_CASE("C ABI: lines_open yields a last line with no trailing newline") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    const dstd::String path = write_text(env.dir() + "/nonl.txt", "a\nb\nlast");

    ::dftu_line_reader* r = nullptr;
    REQUIRE(dftu_test_lines_open(c.get(), &host, path.c_str(), 0, 0, &r) >= 0);
    auto lines = read_lines(c, host, r, 0, nullptr);
    REQUIRE(lines.size() == 3);
    CHECK(lines[2] == "last");
    dftu_test_lines_close(&host, r);
}

TEST_CASE("C ABI: gz writer round trip reads back through lines_open") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    const dstd::String path = env.dir() + "/round.pfw.gz";
    dstd::String text;
    for (int i = 0; i < 40; ++i) text += dstd::fmt<"event-line-{}\n">(i);

    dftu_gz_summary summary{};
    write_gz_from_c(c, host, path, text, 64, &summary);
    CHECK(summary.lines == 40);
    CHECK(summary.uc_bytes == text.size());
    CHECK(summary.members > 1);
    CHECK(summary.c_bytes > 0);

    ::dftu_line_reader* r = nullptr;
    REQUIRE(dftu_test_lines_open(c.get(), &host, path.c_str(), 5, 12, &r) >= 0);
    auto lines = read_lines(c, host, r, 3, nullptr);
    REQUIRE(lines.size() == 8);
    CHECK(lines.front() == "event-line-4");
    CHECK(lines.back() == "event-line-11");
    dftu_test_lines_close(&host, r);
}

TEST_CASE("C ABI: lines_open of a missing file is NOT_FOUND") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    ::dftu_line_reader* r = nullptr;
    const dstd::String path = env.dir() + "/absent.txt";
    CHECK(dftu_test_lines_open(c.get(), &fx.host(), path.c_str(), 0, 0, &r) ==
          DFTU_E_NOT_FOUND);
    CHECK(c.message().find("lines_open") != dstd::String::npos);
}

TEST_CASE("C ABI: lines_open refuses an end line before the start line") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    const dstd::String path = write_text(env.dir() + "/range.txt", "a\nb\n");
    ::dftu_line_reader* r = nullptr;
    CHECK(dftu_test_lines_open(c.get(), &fx.host(), path.c_str(), 3, 2, &r) ==
          DFTU_E_INVALID);
}

TEST_CASE("C ABI: chunks_next reads a file in chunks and ends with END") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    const dstd::String body = "0123456789abcdefghij";
    const dstd::String path = write_text(env.dir() + "/chunks.bin", body);

    ::dftu_chunk_reader* r = nullptr;
    REQUIRE(dftu_test_chunks_open(c.get(), &host, path.c_str(), 8, &r) >= 0);
    dstd::String got;
    int chunks = 0;
    for (;;) {
        dftu_bytes chunk{};
        ::dftu_task* t = nullptr;
        REQUIRE(dftu_test_chunks_next(c.get(), &host, r, &chunk, &t) >= 0);
        const dftu_status status = run(t);
        if (status == DFTU_END) break;
        REQUIRE(status == DFTU_OK);
        CHECK(chunk.len <= 8);
        got.append(static_cast<const char*>(chunk.data),
                   static_cast<std::size_t>(chunk.len));
        ++chunks;
    }
    CHECK(got == body);
    CHECK(chunks == 3);
    dftu_test_chunks_close(&host, r);
}

TEST_CASE("C ABI: chunks_next of a missing file is NOT_FOUND") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    const dstd::String path = env.dir() + "/absent.bin";
    ::dftu_chunk_reader* r = nullptr;
    REQUIRE(dftu_test_chunks_open(c.get(), &host, path.c_str(), 0, &r) >= 0);
    dftu_bytes chunk{};
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_chunks_next(c.get(), &host, r, &chunk, &t) >= 0);
    dstd::String message;
    CHECK(run(t, &message) == DFTU_E_NOT_FOUND);
    CHECK(message.find("chunks_next") != dstd::String::npos);
    dftu_test_chunks_close(&host, r);
}

TEST_CASE("C ABI: gz_append refuses data without a final newline") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    const dstd::String path = env.dir() + "/nonl.pfw.gz";

    ::dftu_gz_writer* w = nullptr;
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_gz_open(c.get(), &host, path.c_str(), 0, &w, &t) >= 0);
    REQUIRE(run(t) == DFTU_OK);
    CHECK(dftu_test_gz_append(c.get(), &host, w, "no newline", 10) ==
          DFTU_E_INVALID);
    CHECK(dftu_test_gz_append(c.get(), &host, w, "ok\n", 3) == DFTU_OK);
    dftu_gz_summary summary{};
    REQUIRE(dftu_test_gz_close(c.get(), &host, w, &summary, &t) >= 0);
    CHECK(run(t) == DFTU_OK);
    CHECK(summary.lines == 1);
    CHECK(dftu_test_gz_append(c.get(), &host, w, "late\n", 5) ==
          DFTU_E_INVALID);
}

TEST_CASE("C ABI: gz_open into a missing directory fails the task") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    const dstd::String path = env.dir() + "/no_such_dir/out.pfw.gz";
    ::dftu_gz_writer* w = nullptr;
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_gz_open(c.get(), &host, path.c_str(), 0, &w, &t) >= 0);
    dstd::String message;
    CHECK(run(t, &message) < 0);
    CHECK(message.find("gz_open") != dstd::String::npos);
    CHECK(dftu_test_gz_append(c.get(), &host, w, "x\n", 2) == DFTU_E_INVALID);
}

TEST_CASE("C ABI: a gz writer left open is aborted when the fold ends") {
    dftu_utils_test::TestEnvironment env(0);
    const dstd::String path = env.dir() + "/abandoned.pfw.gz";
    {
        HostFixture fx;
        Ctx c;
        dftu_plugin_host& host = fx.host();
        ::dftu_gz_writer* w = nullptr;
        ::dftu_task* t = nullptr;
        REQUIRE(dftu_test_gz_open(c.get(), &host, path.c_str(), 0, &w, &t) >=
                0);
        REQUIRE(run(t) == DFTU_OK);
        REQUIRE(dftu_test_gz_append(c.get(), &host, w, "x\n", 2) >= 0);
        REQUIRE(entries_with_prefix(env.dir(), "abandoned.pfw.gz") > 0);
        ::dftu_line_reader* r = nullptr;
        const dstd::String plain = write_text(env.dir() + "/p.txt", "a\n");
        REQUIRE(dftu_test_lines_open(c.get(), &host, plain.c_str(), 0, 0, &r) >=
                0);
    }
    CHECK(entries_with_prefix(env.dir(), "abandoned.pfw.gz") == 0);
}

TEST_CASE("C ABI: compress then decompress round-trips a file") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    dstd::String text;
    for (int i = 0; i < 200; ++i) text += dstd::fmt<"row {}\n">(i);
    const dstd::String plain = write_text(env.dir() + "/in.txt", text);
    const dstd::String gz = env.dir() + "/in.txt.gz";
    const dstd::String back = env.dir() + "/back.txt";

    std::uint64_t gz_size = 0, back_size = 0;
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_compress(c.get(), &host, plain.c_str(), gz.c_str(),
                               &gz_size, &t) >= 0);
    CHECK(run(t) == DFTU_OK);
    CHECK(gz_size > 0);
    CHECK(gz_size == dstd::fs::file_size(dstd::PathView(gz)).value_or(0));
    REQUIRE(dftu_test_decompress(c.get(), &host, gz.c_str(), back.c_str(),
                                 &back_size, &t) >= 0);
    CHECK(run(t) == DFTU_OK);
    CHECK(back_size == text.size());
    CHECK(dstd::fs::read_file(dstd::PathView(back)).value_or(dstd::String()) ==
          text);
}

TEST_CASE("C ABI: compress and decompress of a missing input are NOT_FOUND") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    const dstd::String absent = env.dir() + "/absent.txt";
    const dstd::String out = env.dir() + "/out.gz";
    std::uint64_t size = 0;
    ::dftu_task* t = nullptr;
    dstd::String message;
    REQUIRE(dftu_test_compress(c.get(), &host, absent.c_str(), out.c_str(),
                               &size, &t) >= 0);
    CHECK(run(t, &message) == DFTU_E_NOT_FOUND);
    CHECK(message.find("compress") != dstd::String::npos);
    REQUIRE(dftu_test_decompress(c.get(), &host, absent.c_str(), out.c_str(),
                                 &size, &t) >= 0);
    CHECK(run(t, &message) == DFTU_E_NOT_FOUND);
    CHECK(message.find("decompress") != dstd::String::npos);
}

TEST_CASE("C ABI: the file helpers answer for an existing file") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    const dstd::String path = write_text(env.dir() + "/helper.txt", "12345");

    char logical[1024] = {};
    CHECK(dftu_test_logical_path(c.get(), &host, path.c_str(), logical,
                                 sizeof(logical)) == DFTU_OK);
    CHECK(dstd::String(logical).find("helper.txt") != dstd::String::npos);
    CHECK(logical[0] == '/');
    std::int64_t mtime = 0;
    CHECK(dftu_test_mtime(c.get(), &host, path.c_str(), &mtime) == DFTU_OK);
    CHECK(mtime > 0);
    std::uint64_t size = 0;
    CHECK(dftu_test_size(c.get(), &host, path.c_str(), &size) == DFTU_OK);
    CHECK(size == 5);
    std::uint64_t hash = 0;
    CHECK(dftu_test_sampled_hash(c.get(), &host, path.c_str(), &hash) ==
          DFTU_OK);
    CHECK(hash != 0);
}

TEST_CASE("C ABI: the file helpers report a missing file as NOT_FOUND") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    const dstd::String path = env.dir() + "/absent.txt";
    char logical[64] = {};
    CHECK(dftu_test_logical_path(c.get(), &host, path.c_str(), logical,
                                 sizeof(logical)) == DFTU_E_NOT_FOUND);
    std::int64_t mtime = 0;
    CHECK(dftu_test_mtime(c.get(), &host, path.c_str(), &mtime) ==
          DFTU_E_NOT_FOUND);
    std::uint64_t size = 0;
    CHECK(dftu_test_size(c.get(), &host, path.c_str(), &size) ==
          DFTU_E_NOT_FOUND);
    CHECK(dftu_test_sampled_hash(c.get(), &host, path.c_str(), &size) ==
          DFTU_E_NOT_FOUND);
    CHECK(c.message().find("sampled_hash") != dstd::String::npos);
}

TEST_CASE("C ABI: merge_shards joins shards and reports a missing shard") {
    dftu_utils_test::TestEnvironment env(0);
    HostFixture fx;
    Ctx c;
    dftu_plugin_host& host = fx.host();
    const dstd::String a = write_text(env.dir() + "/m.a", "AA");
    const dstd::String b = write_text(env.dir() + "/m.b", "BB");
    const dstd::String target = env.dir() + "/m.out";
    ::dftu_task* t = nullptr;
    REQUIRE(dftu_test_merge_shards(c.get(), &host, target.c_str(), a.c_str(),
                                   b.c_str(), &t) >= 0);
    CHECK(run(t) == DFTU_OK);
    CHECK(
        dstd::fs::read_file(dstd::PathView(target)).value_or(dstd::String()) ==
        "AABB");

    const dstd::String missing = env.dir() + "/m.absent";
    const dstd::String target2 = env.dir() + "/m2.out";
    dstd::String message;
    REQUIRE(dftu_test_merge_shards(c.get(), &host, target2.c_str(),
                                   missing.c_str(), missing.c_str(), &t) >= 0);
    CHECK(run(t, &message) == DFTU_E_IO);
    CHECK(message.find("merge_shards") != dstd::String::npos);
}
