// SPDX-License-Identifier: MIT
// A duql pipeline that ends in `sink`: the executor binds the plugin sink,
// writes every row the plan streams, and returns the totals as a one-row frame
// with the columns `rows` and `batches`.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dlfcn.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;

#ifndef TOY_SINK_PLUGIN_PATH
#error "TOY_SINK_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

dstd::String event(int i) {
    return R"({"ph":1,"name":"op)" + dstd::to_string(i % 3) +
           R"(","cat":"POSIX","pid":1,"tid":1,"ts":)" +
           dstd::to_string(1000 + 100 * i) + R"(,"dur":)" +
           dstd::to_string(10 + i) + R"(,"args":{}})";
}

// A trace file of `rows` events, indexed, ready for a View.
ViewFile file_of(dftu_utils_test::TestEnvironment& env, int rows) {
    const dstd::String dir = env.dir() + "/f";
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw = dir + "/trace.pfw";
    {
        dftu_utils_test::FileOut o(pfw);
        for (int i = 0; i < rows; ++i) o << event(i) << "\n";
    }
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    dftu_utils_test::ok(
        dftu_utils_test::ok(
            trace::index::Indexer::open({dstd::String(dstd::StringView(gz))}))
            .build());
    ViewFile f;
    f.file_path = gz;
    f.index_path = trace::internal::determine_index_path(gz, "");
    return f;
}

dstd::Optional<plugins::host::Plugins> load(dstd::StringView path) {
    auto loaded =
        plugins::host::Plugins::Builder().add(dstd::String(path)).build();
    if (!loaded) {
        MESSAGE("cannot load ", path, ": ", loaded.error().c_str());
        return dstd::nullopt;
    }
    return dstd::Optional<plugins::host::Plugins>(std::move(*loaded));
}

// The counters the toy sink plugin exports, read from the loaded library.
const std::int64_t* toy_stats(void* handle, std::int64_t* values) {
    using Getter = void (*)(std::int64_t*);
    auto getter = reinterpret_cast<Getter>(dlsym(handle, "toy_sink_stats"));
    if (getter == nullptr) return nullptr;
    getter(values);
    return values;
}

// The library the run loads, already resident.
void* toy_handle() {
    return ::dlopen(TOY_SINK_PLUGIN_PATH, RTLD_LAZY | RTLD_NOLOAD);
}

// The first Int64 value of column `name`.
std::int64_t cell(const dataframe::DataFrame& df, dstd::StringView name) {
    const dataframe::Series* s = nullptr;
    for (std::size_t i = 0; i < df.names.size(); ++i)
        if (dstd::StringView(df.names[i]) == name) s = &df.columns[i];
    if (s == nullptr || s->length() < 1) return -1;
    return s->data<std::int64_t>()[0];
}

}  // namespace

TEST_CASE(
    "a duql pipeline that ends in sink writes the rows and returns the "
    "totals") {
    dstd::Optional<plugins::host::Plugins> plugins = load(TOY_SINK_PLUGIN_PATH);
    if (!plugins) return;

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = View::from_files({file_of(env, 12)});

    const View q =
        v.duql(R"(where dur > 0 | sink "toy_sink.out" path "e2e.csv")");
    auto got = default_runtime().submit(q.collect()).get();
    REQUIRE(got.has_value());
    const dataframe::DataFrame& totals = *got;
    REQUIRE(totals.num_rows() == 1);
    CHECK(cell(totals, "rows") == 12);
    CHECK(cell(totals, "batches") >= 1);
    void* handle = toy_handle();
    REQUIRE(handle != nullptr);
    std::int64_t values[8] = {0};
    CHECK(toy_stats(handle, values) != nullptr);
    CHECK(values[2] >= 1);  // the writer took at least one frame
}

TEST_CASE("a duql pipeline that ends in sink counts each run on its own") {
    dstd::Optional<plugins::host::Plugins> plugins = load(TOY_SINK_PLUGIN_PATH);
    if (!plugins) return;

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = View::from_files({file_of(env, 6)});
    const View q =
        v.duql(R"(where dur > 0 | sink "toy_sink.out" path "again.csv")");

    void* handle = toy_handle();
    REQUIRE(handle != nullptr);
    std::int64_t before[8] = {0};
    const std::int64_t* stats = toy_stats(handle, before);
    REQUIRE(stats != nullptr);
    const std::int64_t writers_before = stats[0];
    for (int run = 0; run < 2; ++run) {
        auto got = default_runtime().submit(q.collect()).get();
        REQUIRE(got.has_value());
        // The second run writes again and reports its own 6 rows, not 12.
        CHECK(cell(*got, "rows") == 6);
        CHECK(cell(*got, "batches") >= 1);
    }
    std::int64_t after[8] = {0};
    const std::int64_t* later = toy_stats(handle, after);
    REQUIRE(later != nullptr);
    CHECK(later[0] - writers_before == 2);  // one bind per run
}

TEST_CASE("duql refuses a stage after sink") {
    dstd::Optional<plugins::host::Plugins> plugins = load(TOY_SINK_PLUGIN_PATH);
    if (!plugins) return;

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = View::from_files({file_of(env, 4)});

    const View q = v.duql(R"(where dur > 0 | sink "toy_sink.out" | take 1)");
    auto got = default_runtime().submit(q.collect()).get();
    REQUIRE_FALSE(got.has_value());
    CHECK(dstd::StringView(got.error().c_str()).find("sink") !=
          dstd::StringView::npos);
}

TEST_CASE("a duql sink no plugin registered fails with the registered list") {
    dstd::Optional<plugins::host::Plugins> plugins = load(TOY_SINK_PLUGIN_PATH);
    if (!plugins) return;

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = View::from_files({file_of(env, 4)});

    const View q = v.duql(R"(where dur > 0 | sink "nope.out" path "x.csv")");
    auto got = default_runtime().submit(q.collect()).get();
    REQUIRE_FALSE(got.has_value());
    const dstd::StringView message(got.error().c_str());
    CHECK(message.find("no sink named 'nope.out'") != dstd::StringView::npos);
    CHECK(message.find("toy_sink.out") != dstd::StringView::npos);
}
