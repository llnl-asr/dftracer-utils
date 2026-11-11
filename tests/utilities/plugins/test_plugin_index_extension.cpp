// SPDX-License-Identifier: MIT
// A plugin index extension (plugins/abi/index.h) built and used for pruning
// through a real .so: tests/utilities/plugins/index_minmax_plugin.c keeps the
// min and max of "v" per chunk as "index_minmax.range".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/domains/plugins/plugins.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
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

using namespace dftracer::utils;
namespace {

constexpr int RECORDS = 3000;
constexpr dstd::StringView EXT = "index_minmax.range";

// Records {"op":"read","v":i} in gzip members of about 8 KB, so the chunks
// hold rising ranges of v.
dstd::String write_records(dftu_utils_test::TestEnvironment& env,
                           const dstd::String& name) {
    const dstd::String plain = env.dir() + "/" + name + ".ndjson";
    {
        dstd::String out;
        for (int i = 0; i < RECORDS; ++i)
            out += common::str_cat(R"({"op":"read","v":)", i, "}\n");
        dstd::fs::write_file(dstd::PathView(plain), out).discard();
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               8 * 1024));
    dstd::fs::remove(plain).discard();
    return gz;
}

domains::plugins::Plugins load(const dstd::String& config = "") {
    auto builder = domains::plugins::Plugins::builder();
    if (config.empty())
        builder.add(INDEX_MINMAX_PLUGIN_PATH);
    else
        builder.add(INDEX_MINMAX_PLUGIN_PATH, config);
    auto set = builder.build();
    REQUIRE(set.has_value());
    return std::move(*set);
}

trace::index::Indexer open(const dstd::String& gz) {
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

trace::views::View view_of(const dstd::String& gz) {
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
        domains::plugins::Plugins set = load();
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
    domains::plugins::Plugins set = load();
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
        domains::plugins::Plugins set = load();
        dftu_utils_test::ok(open(gz).build());
    }
    domains::plugins::Plugins set = load(R"({"version": 2})");
    auto ix = open(gz);
    REQUIRE(entry(ix));
    CHECK_FALSE(entry(ix)->current);
    CHECK(pruned(dftu_utils_test::ok(ix.explain("v > 2990")).at(0)) == nullptr);
    CHECK(dftu_utils_test::ok(ix.status()).needs_work.size() == 1);
}

TEST_CASE("drop and rebuild a plugin extension by name") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_records(env, "d");
    domains::plugins::Plugins set = load();
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
    domains::plugins::Plugins set = load();
    CHECK_FALSE(domains::plugins::Plugins::builder()
                    .add(INDEX_MINMAX_PLUGIN_PATH)
                    .build());
}
