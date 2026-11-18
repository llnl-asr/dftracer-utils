// SPDX-License-Identifier: MIT
// The explain hooks and the run counters of a plugin source, file format and
// node (tests/plugins/explain_plugin.c and chunked_format_plugin.c):
// LazyFrame::explain prints a line for each plugin that has a hook and nothing
// for one that has none, and LazyFrame::explain_analyze runs the plan and
// prints the votes, the rows and the counters of the run.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/provider_registry.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/duql/c_api.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <atomic>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

#ifndef EXPLAIN_PLUGIN_PATH
#error "EXPLAIN_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef CHUNKED_FORMAT_PLUGIN_PATH
#error "CHUNKED_FORMAT_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef SDK_EXPLAIN_PLUGIN_PATH
#error "SDK_EXPLAIN_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

dstd::Optional<plugins::host::Plugins> load(
    std::initializer_list<const char*> paths) {
    plugins::host::Plugins::Builder builder;
    for (const char* path : paths) builder.add(dstd::String(path));
    auto built = builder.build();
    REQUIRE(built.has_value());
    return std::move(built).value();
}

LazyFrame provider(dstd::StringView name) {
    auto factory = find_provider(name);
    REQUIRE(factory != nullptr);
    return LazyFrame::scan(factory->make_source());
}

Expr id_above(std::int64_t value) {
    dftu_scalar s;
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = value;
    return expr_cmp(CmpOp::Gt, col(0), s);
}

dstd::String analyze(const LazyFrame& lf) {
    return dftu_utils_test::ok(
        default_runtime().submit(lf.explain_analyze()).get());
}

bool has(dstd::StringView text, dstd::StringView piece) {
    return text.find(piece) != dstd::StringView::npos;
}

}  // namespace

TEST_CASE("a source with an explain hook prints its lines") {
    auto plugins = load({EXPLAIN_PLUGIN_PATH});
    const dstd::String plan =
        provider("explain_plugin.src").filter(id_above(2)).explain();
    CHECK(has(plan,
              "provider 'explain_plugin.src': filters_offered=1; "
              "filters_accepted=1; columns=2"));
}

TEST_CASE("a source without the hook prints nothing extra") {
    auto plugins = load({EXPLAIN_PLUGIN_PATH});
    const dstd::String plan =
        provider("explain_plugin.quiet").filter(id_above(2)).explain();
    CHECK(plan == "scan [id, v]\nfilter");
}

TEST_CASE("a node with an explain hook prints its lines") {
    auto plugins = load({EXPLAIN_PLUGIN_PATH});
    const dstd::String plan =
        provider("explain_plugin.src").op("explain_plugin.pass", {}).explain();
    CHECK(has(plan, "node 'explain_plugin.pass': mode=pass"));
}

TEST_CASE("explain analyze shows the votes, the rows and the counters") {
    auto plugins = load({EXPLAIN_PLUGIN_PATH});
    const dstd::String text = analyze(provider("explain_plugin.src")
                                          .filter(id_above(2))
                                          .op("explain_plugin.pass", {}));
    CAPTURE(text);
    // The source took the filter as inexact, so the engine kept it.
    CHECK(has(text, "pushed filter 0: inexact"));
    CHECK(has(text, "scan: rows out 6, time (incl. inputs) "));
    CHECK(has(text, "filter: rows in 6, rows out 4, time (incl. inputs) "));
    CHECK(has(
        text,
        "op explain_plugin.pass: rows in 4, rows out 4, time (incl. inputs) "));
    CHECK(has(text, "counter explain_plugin.src.morsels: 2"));
    CHECK(has(text, "counter explain_plugin.src.rows: 6"));
    CHECK(has(text, "counter explain_plugin.src.scans: 1"));
    CHECK(has(text, "counter explain_plugin.pass.frames: 2"));
}

TEST_CASE("explain analyze of a plugin with no counters adds no counter line") {
    auto plugins = load({EXPLAIN_PLUGIN_PATH});
    const dstd::String text =
        analyze(provider("explain_plugin.quiet").filter(id_above(2)));
    CHECK(has(text, "pushed filter 0: inexact"));
    CHECK_FALSE(has(text, "counter "));
    CHECK_FALSE(has(text, "provider "));
}

TEST_CASE("the counters of one analysis do not carry into the next") {
    auto plugins = load({EXPLAIN_PLUGIN_PATH});
    const LazyFrame lf = provider("explain_plugin.src");
    for (int run = 0; run < 2; ++run) {
        const dstd::String text = analyze(lf);
        CHECK(has(text, "counter explain_plugin.src.rows: 6"));
    }
}

TEST_CASE("a counter call outside an analysis changes nothing") {
    auto plugins = load({EXPLAIN_PLUGIN_PATH});
    const LazyFrame lf = provider("explain_plugin.src");
    const DataFrame out =
        dftu_utils_test::ok(default_runtime().submit(lf.collect()).get());
    CHECK(out.num_rows() == 6);
    CHECK(has(analyze(lf), "counter explain_plugin.src.rows: 6"));
}

TEST_CASE("the C ABI explains and analyzes a plan") {
    auto plugins = load({EXPLAIN_PLUGIN_PATH});
    dftu_lazyframe* lf = dftu_lazyframe_from_provider("explain_plugin.src");
    REQUIRE(lf != nullptr);
    char* plain = dftu_lazyframe_explain(lf);
    REQUIRE(plain != nullptr);
    CHECK(has(plain, "provider 'explain_plugin.src'"));
    CHECK_FALSE(has(plain, "analyze:"));
    dftu_duql_string_free(plain);
    char* text = dftu_lazyframe_explain_analyze(lf, 0);
    REQUIRE(text != nullptr);
    CHECK(has(text, "counter explain_plugin.src.rows: 6"));
    dftu_duql_string_free(text);
    dftu_lazyframe_free(lf);
}

namespace {

// The analysis of `lf`, as a result, for a thread that cannot use doctest.
dstd::Result<dstd::String> try_analyze(const LazyFrame& lf) {
    return default_runtime().submit(lf.explain_analyze()).get();
}

}  // namespace

TEST_CASE("two analyses at once on two threads are each exact") {
    auto plugins = load({EXPLAIN_PLUGIN_PATH});
    constexpr int ROUNDS = 40;
    const LazyFrame lf =
        provider("explain_plugin.src").op("explain_plugin.pass", {});
    dstd::Vector<dstd::Result<dstd::String>> got_a;
    dstd::Vector<dstd::Result<dstd::String>> got_b;
    // Both threads arrive before either starts a round, so every round starts
    // its two analyses together.
    std::atomic<int> arrived{0};
    const auto work = [&](dstd::Vector<dstd::Result<dstd::String>>& out) {
        for (int i = 0; i < ROUNDS; ++i) {
            arrived.fetch_add(1);
            while (arrived.load() < 2 * (i + 1)) {
            }
            out.push_back(try_analyze(lf));
        }
    };
    dstd::Thread a([&] { work(got_a); });
    dstd::Thread b([&] { work(got_b); });
    a.join();
    b.join();
    for (const auto* got : {&got_a, &got_b}) {
        REQUIRE(got->size() == ROUNDS);
        for (const auto& r : *got) {
            REQUIRE(r.has_value());
            CHECK(has(*r, "counter explain_plugin.src.rows: 6\n"));
            CHECK(has(*r, "counter explain_plugin.src.morsels: 2\n"));
            CHECK(has(*r, "counter explain_plugin.src.scans: 1"));
            CHECK(has(*r, "counter explain_plugin.pass.frames: 2"));
        }
    }
}

TEST_CASE("an analysis beside a plain run counts only its own") {
    auto plugins = load({EXPLAIN_PLUGIN_PATH});
    const LazyFrame lf = provider("explain_plugin.src");
    std::atomic<bool> stop{false};
    std::atomic<int> plain_runs{0};
    dstd::Thread plain([&] {
        while (!stop.load()) {
            (void)default_runtime().submit(lf.collect()).get();
            plain_runs.fetch_add(1);
        }
    });
    while (plain_runs.load() == 0) {
    }
    int exact = 0;
    for (int i = 0; i < 60; ++i) {
        auto r = try_analyze(lf);
        if (r.has_value() && has(*r, "counter explain_plugin.src.rows: 6\n") &&
            has(*r, "counter explain_plugin.src.scans: 1"))
            ++exact;
    }
    stop.store(true);
    plain.join();
    CHECK(exact == 60);
    CHECK(plain_runs.load() > 0);
}

TEST_CASE("a failed analysis is the error and the next one is exact") {
    auto plugins = load({EXPLAIN_PLUGIN_PATH});
    const LazyFrame bad_scan = provider("explain_plugin.fail");
    const LazyFrame bad_node =
        provider("explain_plugin.src").op("explain_plugin.fail_next", {});
    const LazyFrame good = provider("explain_plugin.src");
    for (int i = 0; i < 3; ++i) {
        auto scan = try_analyze(bad_scan);
        REQUIRE_FALSE(scan.has_value());
        CHECK(has(scan.error().message(), "the scan fails"));
        auto node = try_analyze(bad_node);
        REQUIRE_FALSE(node.has_value());
        CHECK(has(node.error().message(), "next fails"));
        const dstd::String text = analyze(good);
        CHECK(has(text, "counter explain_plugin.src.rows: 6\n"));
        CHECK_FALSE(has(text, "explain_plugin.fail"));
    }
}

TEST_CASE("the partner plan of a join, a cross join and a concat is counted") {
    auto plugins = load({EXPLAIN_PLUGIN_PATH});
    const LazyFrame left = provider("explain_plugin.src");
    const LazyFrame right = provider("explain_plugin.src");
    const dstd::String joined = analyze(left.join(right, {"id"}));
    CHECK(has(joined, "counter explain_plugin.src.rows: 12\n"));
    CHECK(has(joined, "counter explain_plugin.src.scans: 2"));
    const dstd::String crossed =
        analyze(left.join(right, {}, {}, JoinHow::Cross));
    CHECK(has(crossed, "counter explain_plugin.src.rows: 12\n"));
    CHECK(has(crossed, "counter explain_plugin.src.scans: 2"));
    const dstd::String both = analyze(left.concat(right));
    CHECK(has(both, "counter explain_plugin.src.rows: 12\n"));
    CHECK(has(both, "counter explain_plugin.src.scans: 2"));
    const LazyFrame node_side =
        provider("explain_plugin.src").op("explain_plugin.pass", {});
    const dstd::String nodes = analyze(left.concat(node_side));
    CHECK(has(nodes, "counter explain_plugin.pass.frames: 2"));
}

namespace {

struct Scratch {
    dstd::Path root = dftu_utils_test::make_unique_test_path("explain");
    dstd::String data;
    dstd::String index;

    Scratch() {
        data = dstd::String((root / "data").str());
        index = dstd::String((root / "index").str());
        REQUIRE(dstd::fs::create_directories(dstd::PathView(data)).has_value());
        REQUIRE(
            dstd::fs::create_directories(dstd::PathView(index)).has_value());
    }
};

dstd::String write_rows(dstd::StringView dir, dstd::StringView name,
                        dstd::StringView magic, std::int64_t rows) {
    dstd::String text(magic);
    text.push_back('\n');
    for (std::int64_t i = 1; i <= rows; ++i)
        text += dstd::to_string(i) + " n" + dstd::to_string(i) + "\n";
    const dstd::String path = dstd::String(dir) + "/" + dstd::String(name);
    REQUIRE(dstd::fs::write_file(dstd::PathView(path), text).has_value());
    return path;
}

}  // namespace

TEST_CASE("a file format with an explain hook prints its lines") {
    Scratch s;
    auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
    const dstd::String path = write_rows(s.data, "a.rows", "CHK1", 16);
    const dstd::String plan =
        LazyFrame::scan_file(path, s.index).filter(id_above(12)).explain();
    CAPTURE(plan);
    CHECK(has(plan,
              "file format 'chunked_format.rows': chunks=4 of 4 read; "
              "filters_offered=1; chunk_rows=4"));
}

TEST_CASE("a file format without the hook shows only the chunks it reads") {
    Scratch s;
    auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
    const dstd::String path = write_rows(s.data, "a.plain", "PLN1", 16);
    const dstd::String plan = LazyFrame::scan_file(path, s.index).explain();
    CAPTURE(plan);
    CHECK(has(plan, "file format 'chunked_format.plain': chunks=4 of 4 read"));
    CHECK_FALSE(has(plan, "chunk_rows"));
}

TEST_CASE("explain analyze counts the chunks a format scan opened") {
    Scratch s;
    auto plugins = load({CHUNKED_FORMAT_PLUGIN_PATH});
    const dstd::String path = write_rows(s.data, "a.rows", "CHK1", 16);
    const dstd::String text =
        analyze(LazyFrame::scan_file(path, s.index).filter(id_above(12)));
    CAPTURE(text);
    CHECK(has(text, "pushed filter 0: no"));
    CHECK(has(text, "counter chunked_format.rows.chunks: 4"));
    CHECK(has(text, "counter chunked_format.rows.rows: 16"));
}

TEST_CASE("an SDK source and node with explain hooks and counters") {
    auto plugins = load({SDK_EXPLAIN_PLUGIN_PATH});
    const LazyFrame lf = provider("sdk_explain.src").op("sdk_explain.pass", {});
    const dstd::String plan = lf.explain();
    CHECK(has(plan, "provider 'sdk_explain.src': mode=sdk; filters_offered=0"));
    CHECK(has(plan, "node 'sdk_explain.pass': mode=sdk"));
    const dstd::String text = analyze(lf);
    CHECK(has(text, "counter sdk_explain.src.rows: 3"));
    CHECK(has(text, "counter sdk_explain.pass.frames: 1"));
}
