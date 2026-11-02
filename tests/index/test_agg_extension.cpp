// SPDX-License-Identifier: MIT
// The aggregation tier as the dftracer.agg extension: manifest entries, a
// rebuild of only the tier, and readers that use it only when it is current.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/index/build/corrupt_index.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/store/store_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::index::Indexer;
using dftracer::utils::index::IndexerOptions;
using dftracer::utils::index::schemas::dft::agg::AggregationConfig;
using dftracer::utils::index::store::IndexExtension;
using dftracer::utils::trace::views::AggOp;
using dftracer::utils::trace::views::GroupKey;
using dftracer::utils::trace::views::Phase;
using dftracer::utils::trace::views::View;
using dftracer::utils::trace::views::ViewFile;
using dftu_utils_test::TestEnvironment;

namespace {

constexpr std::string_view SENTINEL = "agg_extension.sentinel";

// `n` POSIX reads of 100 us each, one per millisecond from `start_us`.
std::string write_trace(const std::string& dir, const std::string& name, int n,
                        std::uint64_t start_us, bool system = false) {
    const std::string pfw = dir + "/" + name + ".pfw";
    {
        std::ofstream out(pfw.c_str());
        for (int i = 0; i < n; ++i)
            out << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,)"
                   R"("tid":1,"ts":)"
                << start_us + static_cast<std::uint64_t>(i) * 1000
                << R"(,"dur":100,"args":{}})" << "\n";
        for (int i = 0; system && i < n; ++i)
            out << R"({"ph":"C","name":"cpu","cat":"sys","pid":1,"tid":1,)"
                   R"("ts":)"
                << start_us + static_cast<std::uint64_t>(i) * 1000
                << R"(,"args":{"hhash":"h1","util":)" << i << "}}\n";
    }
    const std::string gz = pfw + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
    (void)dstd::fs::remove(pfw);
    return gz;
}

IndexerOptions with_aggregation(std::uint64_t interval_us) {
    IndexerOptions o;
    AggregationConfig c;
    c.time_interval_us = interval_us;
    o.aggregation = c;
    return o;
}

std::string index_of(const std::string& trace) {
    return trace::internal::determine_index_path(trace, "");
}

dataframe::DataFrame counters_of(const std::vector<std::string>& traces) {
    std::vector<ViewFile> files;
    for (const auto& t : traces) files.push_back({t, index_of(t)});
    return View::from_files(std::move(files)).collect_typed().get().counters;
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
void plant_sentinel(const std::string& trace) {
    const auto opened = dftu_utils_test::open_test_index(index_of(trace));
    const auto snapshot = dftu_utils_test::index_take(opened.snapshot());
    const auto file = dftu_utils_test::require_indexed_file(snapshot, trace);
    const auto state =
        index::extension_state(snapshot, file, IndexExtension::Bloom);
    REQUIRE(state.has_value());
    auto build = dftu_utils_test::rebuild_test_file(opened, trace);
    for (const auto& path : dftu_utils_test::index_take(
             index::paths(snapshot, file, IndexExtension::Bloom))) {
        index::put_path(build, IndexExtension::Bloom, path);
        for (const auto& [granule, payload] : dftu_utils_test::index_take(
                 index::granules(snapshot, file, IndexExtension::Bloom, path)))
            index::put_path_granule(build, IndexExtension::Bloom, path, granule,
                                    payload);
        const auto whole = dftu_utils_test::index_take(
            index::path_file(snapshot, file, IndexExtension::Bloom, path));
        if (whole)
            index::put_path_file(build, IndexExtension::Bloom, path, *whole);
    }
    index::put_path_granule(build, IndexExtension::Bloom, SENTINEL, 0, "x");
    build.finish(IndexExtension::Bloom, state->params_hash);
    dftu_utils_test::commit_test_build(build);
}

bool has_sentinel(const std::string& trace) {
    const auto snapshot = dftu_utils_test::index_snapshot(index_of(trace));
    const auto file = dftu_utils_test::require_indexed_file(snapshot, trace);
    return !dftu_utils_test::index_take(index::granules(snapshot, file,
                                                        IndexExtension::Bloom,
                                                        SENTINEL))
                .empty();
}

std::optional<index::ExtensionEntry> agg_entry(const Indexer& ix,
                                               const std::string& trace) {
    const auto manifest = ix.manifest();
    for (const auto& f : manifest)
        if (f.path == trace)
            for (const auto& e : f.extensions)
                if (e.name == "dftracer.agg") return e;
    return std::nullopt;
}

double count_reads(const std::vector<std::string>& traces) {
    std::vector<ViewFile> files;
    for (const auto& t : traces) files.push_back({t, index_of(t)});
    const auto df = View::from_files(std::move(files))
                        .phase(Phase::Events)
                        .group_by({GroupKey::name()})
                        .agg({{AggOp::Count, "", "n"}})
                        .collect()
                        .get();
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
        TestEnvironment env(10);
        const auto gz = write_trace(env.get_dir(), "a", 50, 1000);
        auto ix = Indexer::open({gz}, with_aggregation(1'000'000));
        ix.build();
        const auto e = agg_entry(ix, gz);
        REQUIRE(e);
        CHECK(e->ready);
        CHECK(e->current);
        CHECK(Indexer::open({gz}, with_aggregation(1'000'000))
                  .status()
                  .needs_work.empty());
    }

    TEST_CASE("an unreadable aggregation config is built again") {
        TestEnvironment env(10);
        const auto gz = write_trace(env.get_dir(), "fix", 50, 1000);
        Indexer::open({gz}, with_aggregation(1'000'000)).build();
        CHECK(count_reads({gz}) == 50);
        {
            // A value that does not decode as a config reads as absent.
            namespace kv = dftracer::utils::store;
            auto store = kv::Store::open(
                index::store::internal::normalize_index_root(index_of(gz)) +
                "/store");
            REQUIRE(store.has_value());
            kv::NamespaceSpec spec;
            const auto name =
                index::store::store_layout::agg_meta_namespace_name("config");
            spec.name.assign(name.data(), name.size());
            auto ns = store->ns(spec);
            REQUIRE(ns.has_value());
            const kv::ScopeKey scope = kv::ScopeKey::root();
            const std::string key =
                std::string(1, static_cast<char>(
                                   index::store::layout::agg_kind::META)) +
                "config";
            kv::Batch batch = store->building_batch(*ns, scope);
            batch.put(dstd::ByteView(key), dstd::ByteView("garbage", 7));
            REQUIRE(default_runtime()
                        .submit(store->commit(std::move(batch)))
                        .get()
                        .has_value());
            const kv::Publish target{&*ns, &scope, 0, nullptr};
            REQUIRE(default_runtime()
                        .submit(store->publish(
                            dstd::Span<const kv::Publish>(&target, 1)))
                        .get()
                        .has_value());
        }
        {
            const auto snapshot =
                common::unwrap(index::plan::open_snapshot(index_of(gz)));
            CHECK_FALSE(common::unwrap(
                index::schemas::dft::agg::tier::read_config(snapshot)));
        }
        auto ix = Indexer::open({gz}, with_aggregation(1'000'000));
        ix.build();
        const auto entry = agg_entry(ix, gz);
        REQUIRE(entry);
        CHECK(entry->current);
        CHECK(count_reads({gz}) == 50);
    }

    TEST_CASE("the message for a corrupt index names the directories") {
        const auto once = index::build::corrupt_index_message(
            {"/data/a/.dftindex", "/data/b/.dftindex"}, "Corruption: x", false);
        CHECK(once.find("/data/a/.dftindex") != std::string::npos);
        CHECK(once.find("/data/b/.dftindex") != std::string::npos);
        CHECK(once.find("Corruption: x") != std::string::npos);
        CHECK(once.find("did not repair") == std::string::npos);
        CHECK(once.find("delete that directory and build the index again") !=
              std::string::npos);
        const auto twice = index::build::corrupt_index_message(
            {"/data/a/.dftindex"}, "Corruption: y", true);
        CHECK(twice.find("clearing its aggregation tier did not repair it") !=
              std::string::npos);
        CHECK(twice.find("delete that directory and build the index again") !=
              std::string::npos);
    }

    TEST_CASE("an interval change rebuilds only the tier") {
        TestEnvironment env(10);
        const auto gz = write_trace(env.get_dir(), "a", 50, 1000);
        Indexer::open({gz}, with_aggregation(1'000'000)).build();
        plant_sentinel(gz);

        auto ix = Indexer::open({gz}, with_aggregation(100'000));
        const auto before = ix.status();
        CHECK(before.aggregation_needs_rebuild);
        CHECK(before.aggregation_interval_us == 1'000'000);
        const auto after = ix.build();
        CHECK_FALSE(after.aggregation_needs_rebuild);
        CHECK(after.aggregation_interval_us == 100'000);
        CHECK(after.needs_work.empty());
        CHECK(has_sentinel(gz));
        REQUIRE(agg_entry(ix, gz));
        CHECK(agg_entry(ix, gz)->current);
        CHECK(count_reads({gz}) == 50);
    }

    TEST_CASE("a changed source rebuilds the tier and that file only") {
        TestEnvironment env(10);
        const auto a = write_trace(env.get_dir(), "a", 30, 1000, true);
        const auto b = write_trace(env.get_dir(), "b", 20, 500'000, true);
        Indexer::open({a, b}, with_aggregation(1'000'000)).build();
        CHECK(count_reads({a, b}) == 50);
        plant_sentinel(a);

        write_trace(env.get_dir(), "b", 45, 500'000, true);
        REQUIRE(dstd::fs::set_mtime_ns(
                    b, dstd::fs::mtime_ns(b).value() + 10'000'000'000ULL)
                    .has_value());

        auto ix = Indexer::open({a, b}, with_aggregation(1'000'000));
        // Rebuilding `b` replaces its own partials and column blocks, so no
        // whole-tier rebuild is due.
        CHECK_FALSE(ix.status().aggregation_needs_rebuild);
        ix.build();
        CHECK(has_sentinel(a));
        CHECK(count_reads({a, b}) == 75);
        for (const auto& t : {a, b}) {
            REQUIRE(agg_entry(ix, t));
            CHECK(agg_entry(ix, t)->current);
        }

        // A fresh index over the same files gives the same answer.
        TestEnvironment fresh(10);
        const auto fa = fresh.get_dir() + "/a.pfw.gz";
        const auto fb = fresh.get_dir() + "/b.pfw.gz";
        REQUIRE(dstd::fs::copy_file(a, fa, dstd::fs::CopyMode::FailIfExists)
                    .has_value());
        REQUIRE(dstd::fs::copy_file(b, fb, dstd::fs::CopyMode::FailIfExists)
                    .has_value());
        Indexer::open({fa, fb}, with_aggregation(1'000'000)).build();
        CHECK(count_reads({fa, fb}) == 75);
        const auto rebuilt = counters_of({a, b});
        CHECK(rebuilt.num_rows() > 0);
        CHECK(same_counters(rebuilt, counters_of({fa, fb})));
    }

    TEST_CASE("a file without a current entry makes readers scan") {
        TestEnvironment env(10);
        const auto a = write_trace(env.get_dir(), "a", 30, 1000);
        Indexer::open({a}, with_aggregation(1'000'000)).build();
        // The tier answers this, and its cache keeps the index open, which a
        // later build must still be able to open for writing.
        CHECK(count_reads({a}) == 30);
        const auto b = write_trace(env.get_dir(), "b", 20, 500'000);
        auto ix = Indexer::open({a, b}, with_aggregation(1'000'000));
        ix.build();
        {
            const auto opened = dftu_utils_test::open_test_index(index_of(b));
            const std::string key = dftu_utils_test::index_key(b);
            const dstd::StringView target(key.data(), key.size());
            REQUIRE(default_runtime()
                        .submit(index::remove_extension(
                            opened, IndexExtension::Agg, {&target, 1}))
                        .get()
                        .has_value());
        }
        const auto entry = agg_entry(ix, b);
        CHECK_FALSE((entry && entry->current));
        CHECK(count_reads({a, b}) == 50);
    }
}
