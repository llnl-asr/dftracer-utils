// SPDX-License-Identifier: MIT
// A plugin index extension (plugins/abi/index.h) built and used for pruning
// through a real .so: tests/utilities/plugins/index_minmax_plugin.c keeps the
// min and max of "v" per chunk as "index_minmax.range".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>

#ifndef INDEX_MINMAX_PLUGIN_PATH
#error "INDEX_MINMAX_PLUGIN_PATH must be defined by CMake"
#endif

using namespace dftracer::utils;
namespace {

using dftracer::utils::index::FileExplain;
using dftracer::utils::index::Indexer;
using dftracer::utils::index::IndexerOptions;
using dftracer::utils::plugins::Plugins;
using dftracer::utils::trace::views::ExportSink;
using dftracer::utils::trace::views::View;

constexpr int RECORDS = 3000;
constexpr dstd::StringView EXT = "index_minmax.range";

// Records {"op":"read","v":i} in gzip members of about 8 KB, so the chunks
// hold rising ranges of v.
dstd::String write_records(dftu_utils_test::TestEnvironment& env,
                           const dstd::String& name) {
    const dstd::String plain = env.get_dir() + "/" + name + ".ndjson";
    {
        dstd::String out;
        for (int i = 0; i < RECORDS; ++i)
            out += common::str_cat(R"({"op":"read","v":)", i, "}\n");
        (void)dstd::fs::write_file(dstd::PathView(plain), out);
    }
    const dstd::String gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               8 * 1024));
    (void)dstd::fs::remove(plain);
    return gz;
}

Plugins load(const dstd::String& config = "") {
    auto builder = Plugins::builder();
    if (config.empty())
        builder.add(INDEX_MINMAX_PLUGIN_PATH);
    else
        builder.add(INDEX_MINMAX_PLUGIN_PATH, config);
    auto set = builder.build();
    REQUIRE(set.has_value());
    return std::move(*set);
}

Indexer open(const dstd::String& gz) {
    IndexerOptions o;
    o.extensions = {};
    return Indexer::open({dstd::String(dstd::StringView(gz))}, o);
}

dstd::Optional<dftracer::utils::index::ExtensionEntry> entry(
    const Indexer& ix) {
    const auto manifest = ix.manifest();
    for (const auto& e : manifest.at(0).extensions)
        if (e.name == EXT) return e;
    return dstd::nullopt;
}

const dftracer::utils::index::ExtensionPrune* pruned(const FileExplain& f) {
    for (const auto& e : f.extensions)
        if (e.name == EXT) return &e;
    return nullptr;
}

struct NullSink : ExportSink {
    void write(dstd::StringView) override {}
};

View view_of(const dstd::String& gz) {
    return View::from_file(
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
        Plugins set = load();
        auto ix = open(gz);
        ix.build();
        const auto e = entry(ix);
        REQUIRE(e);
        CHECK(e->ready);
        CHECK(e->current);

        const auto ex = ix.explain(q).at(0);
        chunks = ex.chunks;
        REQUIRE(chunks > 2);
        const auto* p = pruned(ex);
        REQUIRE(p);
        CHECK(p->removed.size() == chunks - 1);
        CHECK(ex.read.size() == 1);
        CHECK_FALSE(ix.explain("v > 5000").at(0).may_match);

        NullSink sink;
        const auto stats = view_of(gz).duql(q).sink_json(sink).get();
        CHECK(stats.events_matched == 9);
        CHECK(stats.chunks_skipped == chunks - 1);
    }
    // The set is gone, so the extension is no longer registered.
    auto ix = open(gz);
    const auto ex = ix.explain(q).at(0);
    CHECK(pruned(ex) == nullptr);
    CHECK(ex.read.size() == chunks);
    CHECK(view_of(gz).duql(q).collect().get().num_rows() == 9);
    CHECK(ix.status().needs_work.empty());
}

TEST_CASE("a file indexed without the plugin needs work once it is loaded") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_records(env, "b");
    open(gz).build();
    Plugins set = load();
    auto ix = open(gz);
    CHECK(ix.status().needs_work.size() == 1);
    ix.build();
    CHECK(ix.status().needs_work.empty());
    REQUIRE(entry(ix));
    CHECK(entry(ix)->current);
}

TEST_CASE("another extension version makes the data stale") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_records(env, "c");
    {
        Plugins set = load();
        open(gz).build();
    }
    Plugins set = load(R"({"version": 2})");
    auto ix = open(gz);
    REQUIRE(entry(ix));
    CHECK_FALSE(entry(ix)->current);
    CHECK(pruned(ix.explain("v > 2990").at(0)) == nullptr);
    CHECK(ix.status().needs_work.size() == 1);
}

TEST_CASE("drop and rebuild a plugin extension by name") {
    dftu_utils_test::TestEnvironment env(10);
    const auto gz = write_records(env, "d");
    Plugins set = load();
    auto ix = open(gz);
    ix.build();
    ix.drop_extension(dstd::String(EXT));
    CHECK_FALSE(entry(ix));
    ix.rebuild_extension(dstd::String(EXT));
    REQUIRE(entry(ix));
    CHECK(entry(ix)->current);
    CHECK_THROWS(ix.rebuild_extension("index_minmax.absent"));
}

TEST_CASE("a second registration of the same name fails to load") {
    Plugins set = load();
    CHECK_FALSE(Plugins::builder().add(INDEX_MINMAX_PLUGIN_PATH).build());
}
