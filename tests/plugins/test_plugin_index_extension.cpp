// SPDX-License-Identifier: MIT
// A plugin index extension (plugins/abi/index.h) built and used for pruning
// through a real .so: tests/plugins/index_minmax_plugin.c keeps the
// min and max of "v" per chunk as "index_minmax.range".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>

#ifndef INDEX_MINMAX_PLUGIN_PATH
#error "INDEX_MINMAX_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef INDEX_COLUMNS_PLUGIN_PATH
#error "INDEX_COLUMNS_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef INDEX_NO_STEP_PLUGIN_PATH
#error "INDEX_NO_STEP_PLUGIN_PATH must be defined by CMake"
#endif

using namespace dftracer::utils;
namespace {

constexpr int RECORDS = 3000;
constexpr dstd::StringView EXT = "index_minmax.range";

// Records {"op":"read","v":i} in gzip members of about 8 KB, so the chunks
// hold rising ranges of v.
dstd::String write_records(dftu_utils_test::TestEnvironment& env,
                           dstd::CStringView name) {
    const dstd::String plain = env.dir() + "/" + name.c_str() + ".ndjson";
    {
        dstd::String out;
        for (int i = 0; i < RECORDS; ++i)
            dstd::fmt_append<"{\"op\":\"read\",\"v\":{}}\n">(out, i);
        dstd::fs::write_file(dstd::PathView(plain), out).discard();
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               8 * 1024));
    dstd::fs::remove(plain).discard();
    return gz;
}

plugins::host::Plugins load(dstd::StringView config = "") {
    auto builder = plugins::host::Plugins::builder();
    if (config.empty())
        builder.add(INDEX_MINMAX_PLUGIN_PATH);
    else
        builder.add(INDEX_MINMAX_PLUGIN_PATH, dstd::String(config));
    auto set = builder.build();
    REQUIRE(set.has_value());
    return std::move(*set);
}

trace::index::Indexer open(dstd::CStringView gz) {
    trace::index::IndexerOptions o;
    o.extensions = {};
    return dftu_utils_test::ok(
        trace::index::Indexer::open({dstd::String(dstd::StringView(gz))}, o));
}

dstd::Optional<dftracer::utils::trace::index::ExtensionEntry> entry(
    const trace::index::Indexer& ix) {
    const auto manifest = dftu_utils_test::ok(ix.manifest());
    for (const auto& e : manifest.at(0).extensions)
        if (e.name == EXT) return e;
    return dstd::nullopt;
}

const dftracer::utils::trace::index::ExtensionPrune* pruned(
    const trace::index::FileExplain& f) {
    for (const auto& e : f.extensions)
        if (e.name == EXT) return &e;
    return nullptr;
}

struct NullSink : trace::views::ExportSink {
    void write(dstd::StringView) override {}
};

trace::views::View view_of(dstd::CStringView gz) {
    return trace::views::View::from_file(
        dstd::String(gz),
        dstd::String(dstd::String(
            dftracer::utils::trace::internal::determine_index_path(gz, ""))));
}

}  // namespace

TEST_CASE("a plugin extension builds, prunes and survives being absent") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_records(env, "a");
    const dstd::String q = "v > 2990";
    std::uint64_t chunks = 0;
    {
        plugins::host::Plugins set = load();
        auto ix = open(gz);
        dftu_utils_test::ok(ix.build());
        const auto e = entry(ix);
        REQUIRE(e);
        CHECK(e->ready);
        CHECK(e->current);

        const auto ex = dftu_utils_test::ok(ix.explain(q)).at(0);
        chunks = ex.chunks;
        REQUIRE(chunks > 2);
        const auto* p = pruned(ex);
        REQUIRE(p);
        CHECK(p->removed.size() == chunks - 1);
        CHECK(ex.read.size() == 1);
        CHECK_FALSE(
            dftu_utils_test::ok(ix.explain("v > 5000")).at(0).may_match);

        NullSink sink;
        const auto stats =
            dftu_utils_test::ok(view_of(gz).duql(q).sink_json(sink).get());
        CHECK(stats.events_matched == 9);
        CHECK(stats.chunks_skipped == chunks - 1);
    }
    // The set is gone, so the extension is no longer registered.
    auto ix = open(gz);
    const auto ex = dftu_utils_test::ok(ix.explain(q)).at(0);
    CHECK(pruned(ex) == nullptr);
    CHECK(ex.read.size() == chunks);
    CHECK(dftu_utils_test::ok(view_of(gz).duql(q).collect().get()).num_rows() ==
          9);
    CHECK(dftu_utils_test::ok(ix.status()).needs_work.empty());
}

TEST_CASE("a file indexed without the plugin needs work once it is loaded") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_records(env, "b");
    dftu_utils_test::ok(open(gz).build());
    plugins::host::Plugins set = load();
    auto ix = open(gz);
    CHECK(dftu_utils_test::ok(ix.status()).needs_work.size() == 1);
    dftu_utils_test::ok(ix.build());
    CHECK(dftu_utils_test::ok(ix.status()).needs_work.empty());
    REQUIRE(entry(ix));
    CHECK(entry(ix)->current);
}

TEST_CASE("another extension version makes the data stale") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_records(env, "c");
    {
        plugins::host::Plugins set = load();
        dftu_utils_test::ok(open(gz).build());
    }
    plugins::host::Plugins set = load(R"({"version": 2})");
    auto ix = open(gz);
    REQUIRE(entry(ix));
    CHECK_FALSE(entry(ix)->current);
    CHECK(pruned(dftu_utils_test::ok(ix.explain("v > 2990")).at(0)) == nullptr);
    CHECK(dftu_utils_test::ok(ix.status()).needs_work.size() == 1);
}

TEST_CASE("drop and rebuild a plugin extension by name") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_records(env, "d");
    plugins::host::Plugins set = load();
    auto ix = open(gz);
    dftu_utils_test::ok(ix.build());
    dftu_utils_test::ok(ix.drop_extension(dstd::String(EXT)));
    CHECK_FALSE(entry(ix));
    dftu_utils_test::ok(ix.rebuild_extension(dstd::String(EXT)));
    REQUIRE(entry(ix));
    CHECK(entry(ix)->current);
    dftu_utils_test::expect_error(ix.rebuild_extension("index_minmax.absent"),
                                  common::ErrorCode::InvalidArgument);
}

TEST_CASE("a second registration of the same name fails to load") {
    plugins::host::Plugins set = load();
    CHECK_FALSE(plugins::host::Plugins::builder()
                    .add(INDEX_MINMAX_PLUGIN_PATH)
                    .build());
}

TEST_CASE("a failing extension step stores nothing and prunes nothing") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_records(env, "e");
    plugins::host::Plugins set = load(R"({"fail_step": true})");
    auto ix = open(gz);
    dftu_utils_test::ok(ix.build());
    const auto ex = dftu_utils_test::ok(ix.explain("v > 2990")).at(0);
    const auto* p = pruned(ex);
    CHECK((p == nullptr || p->removed.empty()));
    CHECK(ex.read.size() == ex.chunks);
    CHECK(dftu_utils_test::ok(view_of(gz).duql("v > 2990").collect().get())
              .num_rows() == 9);
}

namespace {

constexpr dstd::StringView RANGE = "index_columns.range";
constexpr dstd::StringView BLOOM = "index_columns.bloom";

// Events with the int `v` and the string `s` under args. The line extension
// finds "v" by text and the column extension reads the column `args.v`, so
// both see the same numbers. `s` changes every 300 events.
dstd::String write_events(dftu_utils_test::TestEnvironment& env,
                          dstd::CStringView name) {
    const dstd::String plain = env.dir() + "/" + name.c_str() + ".ndjson";
    {
        dstd::String out;
        for (int i = 0; i < RECORDS; ++i)
            dstd::fmt_append<
                "{\"id\":{},\"name\":\"read\",\"cat\":\"POSIX\","
                "\"pid\":1,\"tid\":1,\"ts\":{},\"dur\":1,\"ph\":1,"
                "\"args\":{\"v\":{},\"s\":\"k{}\"}}\n">(out, i, i, i, i / 300);
        dstd::fs::write_file(dstd::PathView(plain), out).discard();
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               8 * 1024));
    dstd::fs::remove(plain).discard();
    return gz;
}

plugins::host::Plugins load_both() {
    auto builder = plugins::host::Plugins::builder();
    builder.add(INDEX_MINMAX_PLUGIN_PATH);
    builder.add(INDEX_COLUMNS_PLUGIN_PATH);
    auto set = builder.build();
    REQUIRE(set.has_value());
    return std::move(*set);
}

dstd::Optional<trace::index::ExtensionEntry> entry_of(
    const trace::index::Indexer& ix, dstd::StringView name) {
    const auto manifest = dftu_utils_test::ok(ix.manifest());
    for (const auto& e : manifest.at(0).extensions)
        if (e.name == name) return e;
    return dstd::nullopt;
}

const trace::index::ExtensionPrune* prune_of(const trace::index::FileExplain& f,
                                             dstd::StringView name) {
    for (const auto& e : f.extensions)
        if (e.name == name) return &e;
    return nullptr;
}

}  // namespace

TEST_CASE("a column extension prunes the chunks the line extension prunes") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_events(env, "cols");
    plugins::host::Plugins set = load_both();
    auto ix = open(gz);
    dftu_utils_test::ok(ix.build());
    REQUIRE(entry_of(ix, RANGE));
    CHECK(entry_of(ix, RANGE)->ready);
    CHECK(entry_of(ix, RANGE)->current);

    const auto lines = dftu_utils_test::ok(ix.explain("v > 2990")).at(0);
    const auto cols = dftu_utils_test::ok(ix.explain("args.v > 2990")).at(0);
    REQUIRE(lines.chunks > 2);
    const auto* by_lines = prune_of(lines, EXT);
    const auto* by_cols = prune_of(cols, RANGE);
    REQUIRE(by_lines);
    REQUIRE(by_cols);
    // The column extension compiles the leaf only when the host says the
    // column is an int64, so this also shows that the leaf's type is filled.
    CHECK(by_cols->removed.size() == lines.chunks - 1);
    CHECK(by_cols->removed == by_lines->removed);
    CHECK(dftu_utils_test::ok(ix.explain("args.v > 5000")).at(0).may_match ==
          false);
}

TEST_CASE("a bloom of host hashes prunes by value and keeps every match") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_events(env, "bloom");
    plugins::host::Plugins set = load_both();
    auto ix = open(gz);
    dftu_utils_test::ok(ix.build());
    REQUIRE(entry_of(ix, BLOOM));
    CHECK(entry_of(ix, BLOOM)->current);

    // The string `s` is "k9" for the events 2700 to 2999.
    const auto ex = dftu_utils_test::ok(ix.explain("args.s == \"k9\"")).at(0);
    const auto* p = prune_of(ex, BLOOM);
    REQUIRE(p);
    CHECK(p->removed.size() + 6 >= ex.chunks);
    CHECK(p->removed.size() < ex.chunks);
    CHECK(dftu_utils_test::ok(
              view_of(gz).duql("args.s == \"k9\"").collect().get())
              .num_rows() == 300);
    // A value no event holds has no chunk to read.
    const auto none =
        dftu_utils_test::ok(ix.explain("args.s == \"absent\"")).at(0);
    CHECK(prune_of(none, BLOOM)->removed.size() == none.chunks);
}

TEST_CASE("an index extension with neither step nor step_column is refused") {
    auto set = plugins::host::Plugins::builder()
                   .add(INDEX_NO_STEP_PLUGIN_PATH)
                   .build();
    REQUIRE_FALSE(set.has_value());
    const dstd::String message(set.error().message());
    CHECK(message.find("index_no_step.none") != dstd::String::npos);
    CHECK(message.find("neither step nor step_column") != dstd::String::npos);
}

namespace {

constexpr dstd::StringView NEVER = "index_columns.never";
constexpr dstd::StringView STAR = "index_columns.star";

plugins::host::Plugins load_columns() {
    auto builder = plugins::host::Plugins::builder();
    builder.add(INDEX_COLUMNS_PLUGIN_PATH);
    auto set = builder.build();
    REQUIRE(set.has_value());
    return std::move(*set);
}

// Genesis records: the schema marks `dur.sketch` never_index, so the index
// build decodes no value of it.
dstd::String write_genesis(dftu_utils_test::TestEnvironment& env,
                           dstd::CStringView name) {
    const dstd::String plain = env.dir() + "/" + name.c_str() + ".ndjson";
    {
        dstd::String out;
        for (int i = 0; i < 600; ++i)
            dstd::fmt_append<
                "{\"gtype\":\"func\",\"run\":\"ab\",\"path\":\"main;f\","
                "\"ts\":1,\"dur\":{\"sketch\":\"SK{}\",\"count\":2}}\n">(out,
                                                                         i);
        dstd::fs::write_file(dstd::PathView(plain), out).discard();
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               4 * 1024));
    dstd::fs::remove(plain).discard();
    return gz;
}

std::size_t rows(dstd::CStringView gz, dstd::StringView query) {
    return dftu_utils_test::ok(
               view_of(gz).duql(dstd::String(query)).collect().get())
        .num_rows();
}

// Variants of args.v over events 1000 to 1299 (`kind`): 0 leaves it out, 1
// alternates ints and floats, 2 alternates ints and nulls.
dstd::String write_untidy(dftu_utils_test::TestEnvironment& env,
                          dstd::CStringView name, int kind) {
    const dstd::String plain = env.dir() + "/" + name.c_str() + ".ndjson";
    {
        dstd::String out;
        for (int i = 0; i < RECORDS; ++i) {
            dstd::String args;
            const bool untidy = i >= 1000 && i < 1300;
            if (!untidy || (kind == 1 && i % 2 == 0) ||
                (kind == 2 && i % 2 == 0))
                args = dstd::fmt<"\"v\":{},">(i);
            else if (kind == 1)
                args = dstd::fmt<"\"v\":{}.5,">(i);
            else if (kind == 2)
                args = "\"v\":null,";
            dstd::fmt_append<
                "{\"id\":{},\"name\":\"read\",\"cat\":\"POSIX\","
                "\"pid\":1,\"tid\":1,\"ts\":{},\"dur\":1,\"ph\":1,"
                "\"args\":{{}\"s\":\"k\"}}\n">(out, i, i, args);
        }
        dstd::fs::write_file(dstd::PathView(plain), out).discard();
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               8 * 1024));
    dstd::fs::remove(plain).discard();
    return gz;
}

}  // namespace

TEST_CASE(
    "a column named for a never_index path stores nothing and prunes none") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_genesis(env, "never");
    // `index_columns.never` prunes a chunk it was passed `dur.sketch` for when
    // `ts == 1` is asked. That is the case where the column must not be passed:
    // the index holds no value of that path, so nothing may be stored.
    const dstd::StringView q = "ts == 1";
    std::size_t scan_rows = 0;
    {
        plugins::host::Plugins set = load_columns();
        auto ix = open(gz);
        dftu_utils_test::ok(ix.build());
        const auto ex = dftu_utils_test::ok(ix.explain(dstd::String(q))).at(0);
        REQUIRE(ex.chunks > 2);
        const auto* p = prune_of(ex, NEVER);
        CHECK((p == nullptr || p->removed.empty()));
        CHECK(ex.read.size() == ex.chunks);
        scan_rows = rows(gz, q);
        CHECK(scan_rows == 600);
    }
    CHECK(rows(gz, q) == scan_rows);
}

TEST_CASE("a star extension skips never_index paths and keeps the others") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_genesis(env, "star");
    plugins::host::Plugins set = load_columns();
    auto ix = open(gz);
    dftu_utils_test::ok(ix.build());
    // The chunks are stored, so the file is not failed: `ts` was passed in
    // every chunk, while the never_index path was never asked for by name.
    const auto ts = dftu_utils_test::ok(ix.explain("ts == 1")).at(0);
    const auto* p = prune_of(ts, STAR);
    CHECK((p == nullptr || p->removed.empty()));
    const auto sk =
        dftu_utils_test::ok(ix.explain("dur.sketch == \"SK7\"")).at(0);
    const auto* q = prune_of(sk, STAR);
    REQUIRE(q);
    CHECK(q->removed.size() == sk.chunks);
}

TEST_CASE("untidy args.v is never pruned wrongly") {
    struct Case {
        int kind;
        const char* label;
    };
    for (const Case c :
         {Case{0, "absent"}, Case{1, "mixed"}, Case{2, "null"}}) {
        CAPTURE(c.label);
        dftu_utils_test::TestEnvironment env(10);
        const auto gz = write_untidy(env, "untidy", c.kind);
        const dstd::StringView queries[] = {"args.v > 2990", "args.v < 5",
                                            "args.v == 1101.5",
                                            "args.v == 1100", "args.v >= 1000"};
        std::size_t with[5] = {};
        {
            plugins::host::Plugins set = load_columns();
            auto ix = open(gz);
            dftu_utils_test::ok(ix.build());
            const auto ex =
                dftu_utils_test::ok(ix.explain("args.v > 2990")).at(0);
            const auto* p = prune_of(ex, RANGE);
            REQUIRE(p);
            const std::size_t unpruned = ex.chunks - p->removed.size();
            if (c.kind == 1)
                // A chunk with ints and floats has no column: it is kept.
                CHECK(unpruned >= 4);
            else
                // Every chunk but the last proves no match, also the ones
                // with no value or only nulls.
                CHECK(unpruned == 1);
            for (int i = 0; i < 5; ++i) with[i] = rows(gz, queries[i]);
        }
        for (int i = 0; i < 5; ++i) {
            CAPTURE(queries[i]);
            CHECK(rows(gz, queries[i]) == with[i]);
        }
        CHECK(with[0] == 9);
    }
}
