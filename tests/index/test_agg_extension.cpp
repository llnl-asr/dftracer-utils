// SPDX-License-Identifier: MIT
// The aggregation tier as the dftracer.agg extension: manifest entries, a
// rebuild of only the tier, and readers that use it only when it is current.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/index/build/corrupt_index.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/trace/index/store/layout.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/store/store_layout.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <chrono>
#include <cmath>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::index;

namespace {

constexpr dstd::StringView SENTINEL = "agg_extension.sentinel";

// `n` POSIX reads of 100 us each, one per millisecond from `start_us`.
dstd::String write_trace(const dstd::String& dir, const dstd::String& name,
                         int n, std::uint64_t start_us, bool system = false) {
    const dstd::String pfw = dir + "/" + name + ".pfw";
    {
        dstd::String out;
        for (int i = 0; i < n; ++i)
            out += common::str_cat(
                R"({"ph":"X","name":"read","cat":"POSIX","pid":1,)"
                R"("tid":1,"ts":)",
                start_us + static_cast<std::uint64_t>(i) * 1000,
                R"(,"dur":100,"args":{}})", "\n");
        for (int i = 0; system && i < n; ++i)
            out += common::str_cat(
                R"({"ph":"C","name":"cpu","cat":"sys","pid":1,"tid":1,)"
                R"("ts":)",
                start_us + static_cast<std::uint64_t>(i) * 1000,
                R"(,"args":{"hhash":"h1","util":)", i, "}}\n");
        dstd::fs::write_file(dstd::PathView(pfw), out).discard();
    }
    const dstd::String gz = pfw + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
    dstd::fs::remove(pfw).discard();
    return gz;
}

IndexerOptions with_aggregation(std::uint64_t interval_us) {
    IndexerOptions o;
    schemas::dft::agg::AggregationConfig c;
    c.time_interval_us = interval_us;
    o.aggregation = c;
    return o;
}

dstd::String index_of(const dstd::String& trace) {
    return dstd::String(trace::internal::determine_index_path(trace, ""));
}

dataframe::DataFrame counters_of(const dstd::Vector<dstd::String>& traces) {
    dstd::Vector<trace::views::ViewFile> files;
    for (const auto& t : traces)
        files.push_back({dstd::String(t), dstd::String(index_of(t))});
    return dftu_utils_test::ok(trace::views::View::from_files(std::move(files))
                                   .collect_typed()
                                   .get())
        .counters;
}

bool same_counters(const dataframe::DataFrame& a,
                   const dataframe::DataFrame& b) {
    if (a.names != b.names || a.num_rows() != b.num_rows()) return false;
    for (std::size_t c = 0; c < a.columns.size(); ++c) {
        const auto x = a.columns[c].materialize();
        const auto y = b.columns[c].materialize();
        if (x.type() != y.type()) return false;
        for (std::int64_t r = 0; r < a.num_rows(); ++r) {
            if (x.is_null(r) != y.is_null(r)) return false;
            if (x.type() == dataframe::TypeId::String) {
                if (x.string_at(r) != y.string_at(r)) return false;
            } else {
                const double xv = x.data<double>()[r];
                const double yv = y.data<double>()[r];
                if (xv != yv && !(std::isnan(xv) && std::isnan(yv)))
                    return false;
            }
        }
    }
    return true;
}

// A bloom granule no build writes: it survives only while the file's bloom
// data is not rewritten, so the rewrite keeps everything the file holds.
void plant_sentinel(const dstd::String& trace) {
    const auto opened = dftu_utils_test::open_test_index(index_of(trace));
    const auto snapshot = dftu_utils_test::index_take(opened.snapshot());
    const auto file = dftu_utils_test::require_indexed_file(snapshot, trace);
    const auto state = trace::index::extension_state(
        snapshot, file, trace::index::store::IndexExtension::Bloom);
    REQUIRE(state.has_value());
    auto build = dftu_utils_test::rebuild_test_file(opened, trace);
    for (const auto& path : dftu_utils_test::index_take(trace::index::paths(
             snapshot, file, trace::index::store::IndexExtension::Bloom))) {
        trace::index::put_path(
            build, trace::index::store::IndexExtension::Bloom, path);
        for (const auto& [granule, payload] :
             dftu_utils_test::index_take(trace::index::granules(
                 snapshot, file, trace::index::store::IndexExtension::Bloom,
                 path)))
            trace::index::put_path_granule(
                build, trace::index::store::IndexExtension::Bloom, path,
                granule, payload);
        const auto whole = dftu_utils_test::index_take(trace::index::path_file(
            snapshot, file, trace::index::store::IndexExtension::Bloom, path));
        if (whole)
            trace::index::put_path_file(
                build, trace::index::store::IndexExtension::Bloom, path,
                *whole);
    }
    trace::index::put_path_granule(
        build, trace::index::store::IndexExtension::Bloom, SENTINEL, 0, "x");
    build.finish(trace::index::store::IndexExtension::Bloom,
                 state->params_hash);
    dftu_utils_test::commit_test_build(build);
}

bool has_sentinel(const dstd::String& trace) {
    const auto snapshot = dftu_utils_test::index_snapshot(index_of(trace));
    const auto file = dftu_utils_test::require_indexed_file(snapshot, trace);
    return !dftu_utils_test::index_take(
                trace::index::granules(
                    snapshot, file, trace::index::store::IndexExtension::Bloom,
                    SENTINEL))
                .empty();
}

dstd::Optional<trace::index::ExtensionEntry> agg_entry(
    const Indexer& ix, const dstd::String& trace) {
    const auto manifest = dftu_utils_test::ok(ix.manifest());
    for (const auto& f : manifest)
        if (dstd::StringView(f.path) == dstd::StringView(trace))
            for (const auto& e : f.extensions)
                if (e.name == "dftracer.agg") return e;
    return dstd::nullopt;
}

double count_reads(const dstd::Vector<dstd::String>& traces) {
    dstd::Vector<trace::views::ViewFile> files;
    for (const auto& t : traces)
        files.push_back({dstd::String(t), dstd::String(index_of(t))});
    const auto df =
        dftu_utils_test::ok(trace::views::View::from_files(std::move(files))
                                .phase(trace::views::Phase::Events)
                                .group_by({trace::views::GroupKey::name()})
                                .agg({{trace::views::AggOp::Count, "", "n"}})
                                .collect()
                                .get());
    REQUIRE(df.num_rows() == 1);
    for (std::size_t i = 0; i < df.names.size(); ++i)
        if (df.names[i] == "n") {
            const auto& c = df.columns[i];
            if (c.type() == dataframe::TypeId::Int64)
                return static_cast<double>(c.data<std::int64_t>()[0]);
            if (c.type() == dataframe::TypeId::Uint64)
                return static_cast<double>(c.data<std::uint64_t>()[0]);
            return c.data<double>()[0];
        }
    FAIL("no count column");
    return 0;
}

}  // namespace

TEST_SUITE("AggExtension") {
    TEST_CASE("an aggregated file lists a current dftracer.agg entry") {
        dftu_utils_test::TestEnvironment env(10);
        const auto gz = write_trace(env.dir(), "a", 50, 1000);
        auto ix = dftu_utils_test::ok(Indexer::open(
            {dstd::String(dstd::StringView(gz))}, with_aggregation(1'000'000)));
        dftu_utils_test::ok(ix.build());
        const auto e = agg_entry(ix, gz);
        REQUIRE(e);
        CHECK(e->ready);
        CHECK(e->current);
        CHECK(dftu_utils_test::ok(
                  dftu_utils_test::ok(
                      Indexer::open({dstd::String(dstd::StringView(gz))},
                                    with_aggregation(1'000'000)))
                      .status())
                  .needs_work.empty());
    }

    TEST_CASE("an unreadable aggregation config is built again") {
        dftu_utils_test::TestEnvironment env(10);
        const auto gz = write_trace(env.dir(), "fix", 50, 1000);
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(gz))},
                              with_aggregation(1'000'000)))
                .build());
        CHECK(count_reads({gz}) == 50);
        {
            // A value that does not decode as a config reads as absent.
            auto store = dftracer::utils::store::Store::open(
                trace::index::normalize_index_root(index_of(gz)) + "/store");
            REQUIRE(store.has_value());
            dftracer::utils::store::NamespaceSpec spec;
            const auto name =
                trace::index::store::store_layout::agg_meta_namespace_name(
                    "config");
            spec.name.assign(name.data(), name.size());
            auto ns = store->ns(spec);
            REQUIRE(ns.has_value());
            const dftracer::utils::store::ScopeKey scope =
                dftracer::utils::store::ScopeKey::root();
            const dstd::String key =
                dstd::String(1,
                             static_cast<char>(
                                 trace::index::store::layout::agg_kind::META)) +
                "config";
            dftracer::utils::store::Batch batch =
                store->building_batch(*ns, scope);
            batch.put(dstd::ByteView(key), dstd::ByteView("garbage", 7));
            REQUIRE(default_runtime()
                        .submit(store->commit(std::move(batch)))
                        .get()
                        .has_value());
            const dftracer::utils::store::Publish target{&*ns, &scope, 0,
                                                         nullptr};
            REQUIRE(default_runtime()
                        .submit(store->publish(
                            dstd::Span<const dftracer::utils::store::Publish>(
                                &target, 1)))
                        .get()
                        .has_value());
        }
        {
            const auto snapshot = dftu_utils_test::ok(
                trace::index::plan::open_snapshot(index_of(gz)));
            CHECK_FALSE(dftu_utils_test::ok(
                trace::index::schemas::dft::agg::tier::read_config(snapshot)));
        }
        auto ix = dftu_utils_test::ok(Indexer::open(
            {dstd::String(dstd::StringView(gz))}, with_aggregation(1'000'000)));
        dftu_utils_test::ok(ix.build());
        const auto entry = agg_entry(ix, gz);
        REQUIRE(entry);
        CHECK(entry->current);
        CHECK(count_reads({gz}) == 50);
    }

    TEST_CASE("the message for a corrupt index names the directories") {
        const auto once = trace::index::build::corrupt_index_message(
            {"/data/a/.dftindex", "/data/b/.dftindex"}, "Corruption: x", false);
        CHECK(once.find("/data/a/.dftindex") != dstd::String::npos);
        CHECK(once.find("/data/b/.dftindex") != dstd::String::npos);
        CHECK(once.find("Corruption: x") != dstd::String::npos);
        CHECK(once.find("did not repair") == dstd::String::npos);
        CHECK(once.find("delete that directory and build the index again") !=
              dstd::String::npos);
        const auto twice = trace::index::build::corrupt_index_message(
            {"/data/a/.dftindex"}, "Corruption: y", true);
        CHECK(twice.find("clearing its aggregation tier did not repair it") !=
              dstd::String::npos);
        CHECK(twice.find("delete that directory and build the index again") !=
              dstd::String::npos);
    }

    TEST_CASE("an interval change rebuilds only the tier") {
        dftu_utils_test::TestEnvironment env(10);
        const auto gz = write_trace(env.dir(), "a", 50, 1000);
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(gz))},
                              with_aggregation(1'000'000)))
                .build());
        plant_sentinel(gz);

        auto ix = dftu_utils_test::ok(Indexer::open(
            {dstd::String(dstd::StringView(gz))}, with_aggregation(100'000)));
        const auto before = dftu_utils_test::ok(ix.status());
        CHECK(before.aggregation_needs_rebuild);
        CHECK(before.aggregation_interval_us == 1'000'000);
        const auto after = dftu_utils_test::ok(ix.build());
        CHECK_FALSE(after.aggregation_needs_rebuild);
        CHECK(after.aggregation_interval_us == 100'000);
        CHECK(after.needs_work.empty());
        CHECK(has_sentinel(gz));
        REQUIRE(agg_entry(ix, gz));
        CHECK(agg_entry(ix, gz)->current);
        CHECK(count_reads({gz}) == 50);
    }

    TEST_CASE("a changed source rebuilds the tier and that file only") {
        dftu_utils_test::TestEnvironment env(10);
        const auto a = write_trace(env.dir(), "a", 30, 1000, true);
        const auto b = write_trace(env.dir(), "b", 20, 500'000, true);
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(a)),
                               dstd::String(dstd::StringView(b))},
                              with_aggregation(1'000'000)))
                .build());
        CHECK(count_reads({a, b}) == 50);
        plant_sentinel(a);

        write_trace(env.dir(), "b", 45, 500'000, true);
        REQUIRE(dstd::fs::set_mtime_ns(
                    b, dstd::fs::mtime_ns(b).value() + 10'000'000'000ULL)
                    .has_value());

        auto ix = dftu_utils_test::ok(
            Indexer::open({dstd::String(dstd::StringView(a)),
                           dstd::String(dstd::StringView(b))},
                          with_aggregation(1'000'000)));
        // Rebuilding `b` replaces its own partials and column blocks, so no
        // whole-tier rebuild is due.
        CHECK_FALSE(dftu_utils_test::ok(ix.status()).aggregation_needs_rebuild);
        dftu_utils_test::ok(ix.build());
        CHECK(has_sentinel(a));
        CHECK(count_reads({a, b}) == 75);
        for (const auto& t : {a, b}) {
            REQUIRE(agg_entry(ix, t));
            CHECK(agg_entry(ix, t)->current);
        }

        // A fresh index over the same files gives the same answer.
        dftu_utils_test::TestEnvironment fresh(10);
        const auto fa = fresh.dir() + "/a.pfw.gz";
        const auto fb = fresh.dir() + "/b.pfw.gz";
        REQUIRE(dstd::fs::copy_file(a, fa, dstd::fs::CopyMode::FailIfExists)
                    .has_value());
        REQUIRE(dstd::fs::copy_file(b, fb, dstd::fs::CopyMode::FailIfExists)
                    .has_value());
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(fa)),
                               dstd::String(dstd::StringView(fb))},
                              with_aggregation(1'000'000)))
                .build());
        CHECK(count_reads({fa, fb}) == 75);
        const auto rebuilt = counters_of({a, b});
        CHECK(rebuilt.num_rows() > 0);
        CHECK(same_counters(rebuilt, counters_of({fa, fb})));
    }

    TEST_CASE("a file without a current entry makes readers scan") {
        dftu_utils_test::TestEnvironment env(10);
        const auto a = write_trace(env.dir(), "a", 30, 1000);
        dftu_utils_test::ok(
            dftu_utils_test::ok(
                Indexer::open({dstd::String(dstd::StringView(a))},
                              with_aggregation(1'000'000)))
                .build());
        // The tier answers this, and its cache keeps the index open, which a
        // later build must still be able to open for writing.
        CHECK(count_reads({a}) == 30);
        const auto b = write_trace(env.dir(), "b", 20, 500'000);
        auto ix = dftu_utils_test::ok(
            Indexer::open({dstd::String(dstd::StringView(a)),
                           dstd::String(dstd::StringView(b))},
                          with_aggregation(1'000'000)));
        dftu_utils_test::ok(ix.build());
        {
            const auto opened = dftu_utils_test::open_test_index(index_of(b));
            const auto key = dftu_utils_test::index_key(b);
            const dstd::StringView target(key.data(), key.size());
            REQUIRE(default_runtime()
                        .submit(trace::index::remove_extension(
                            opened, trace::index::store::IndexExtension::Agg,
                            {&target, 1}))
                        .get()
                        .has_value());
        }
        const auto entry = agg_entry(ix, b);
        CHECK_FALSE((entry && entry->current));
        CHECK(count_reads({a, b}) == 50);
    }
}
