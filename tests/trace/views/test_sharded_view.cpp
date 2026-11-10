// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/versions.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/store/shard_manifest.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/views/sharded_view.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_runtime.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <fstream>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

using namespace dftracer::utils::trace;

namespace {

// A ph="X" trace in its own subdir, so each shard's sidecar index is distinct.
dstd::String make_x_trace(TestEnvironment& env, const dstd::String& tag,
                          int posix_n, int stdio_n) {
    dstd::String dir = env.dir() + "/" + tag;
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    dstd::String pfw = dir + "/" + tag + ".pfw";
    dftu_utils_test::FileOut ofs(pfw);
    for (int i = 0; i < posix_n; ++i)
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i) << R"(,"args":{}})"
            << "\n";
    for (int i = 0; i < stdio_n; ++i)
        ofs << R"({"ph":"X","name":"fwrite","cat":"STDIO","pid":1,"tid":1,"ts":)"
            << (5000 + i * 100) << R"(,"dur":)" << (20 + i) << R"(,"args":{}})"
            << "\n";
    ofs.close();
    dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    return gz;
}

// Build a full sidecar index including the aggregation tier the way the
// aggregator does, so the chunk partials can answer without a scan. `index_dir`
// names the index root to build into, such as a shard root.
void build_shard_index(
    const dstd::String& gz,
    const index::schemas::dft::agg::AggregationConfig& config = {},
    const dstd::String& index_dir = "") {
    dftracer::utils::index::IndexerOptions options;
    options.aggregation = config;
    options.index_dir = index_dir;
    dftu_utils_test::ok(
        dftu_utils_test::ok(dftracer::utils::index::Indexer::open(
                                {dstd::String(dstd::StringView(gz))}, options))
            .build());
}

// The trace files the root's store holds a current index for.
dstd::Vector<dstd::String> indexed_files(
    const dstd::String& root, const dstd::Vector<dstd::String>& files) {
    dftracer::utils::index::IndexerOptions options;
    options.index_dir = dstd::String(dstd::StringView(root));
    options.aggregation = index::schemas::dft::agg::AggregationConfig{};
    dstd::Vector<dstd::String> paths;
    for (const auto& file : files) paths.emplace_back(file);
    auto status = dftu_utils_test::ok(
        dftu_utils_test::ok(
            dftracer::utils::index::Indexer::open(std::move(paths), options))
            .status());
    REQUIRE(status.needs_work.empty());
    dstd::Vector<dstd::String> ready;
    for (const auto& file : status.ready) ready.emplace_back(file);
    return ready;
}

// Render a DataFrame canonically (String columns are the group keys, numeric
// columns the values) for order-independent comparison.
dstd::Vector<dstd::String> canon(const dataframe::DataFrame& b) {
    dstd::Vector<dstd::String> rows;
    for (std::int64_t i = 0; i < b.num_rows(); ++i) {
        dstd::String s;
        for (const auto& c : b.columns) {
            if (c.type() == dataframe::TypeId::String) {
                s += dstd::String(c.string_at(i)) + "|";
            } else {
                double v = (c.type() == dataframe::TypeId::Int64)
                               ? static_cast<double>(c.data<std::int64_t>()[i])
                           : (c.type() == dataframe::TypeId::Uint64)
                               ? static_cast<double>(c.data<std::uint64_t>()[i])
                               : c.data<double>()[i];
                s += dstd::to_string(v) + "|";
            }
        }
        rows.push_back(std::move(s));
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

struct QueryOut {
    struct Row {
        std::uint64_t count = 0;
        double sum = 0;
        double p50 = 0;
        double p90 = 0;
    };
    dstd::Map<dstd::String, Row> rows;
    std::size_t chunks_reread = 0;
};

QueryOut run_query(const dftracer::utils::trace::AggQuery& q) {
    QueryOut out;
    dstd::String error;
    dftu_utils_test::run_coro(
        [&](coro::Scope&) -> dftracer::utils::coro::CoroTask<void> {
            auto r = co_await dftracer::utils::trace::run_agg_query(q);
            if (!r) {
                error = r.error().format();
                co_return;
            }
            out.chunks_reread = r->stats.chunks_reread;
            for (const auto& [k, p] : r->rows)
                out.rows[dstd::String(r->text(k.cat)) + "|" +
                         dstd::String(r->text(k.name))] = {
                    p.count, p.duration.sum, p.duration_q.quantile(0.5),
                    p.duration_q.quantile(0.9)};
            co_return;
        },
        "sharded-view-agg-query");
    REQUIRE_MESSAGE(error.empty(), error);
    return out;
}

void check_merged_partials(
    const dstd::String& tag,
    const index::schemas::dft::agg::AggregationConfig& config) {
    TestEnvironment env(200);
    REQUIRE(env.is_valid());
    dstd::String a = make_x_trace(env, tag + "alpha", 30, 20);
    dstd::String b = make_x_trace(env, tag + "beta", 10, 40);
    dstd::String ia = dstd::String(determine_index_path(a, ""));
    dstd::String ib = dstd::String(determine_index_path(b, ""));
    build_shard_index(a, config);
    build_shard_index(b, config);

    dstd::String root = env.dir() + "/set";
    dftu_utils_test::ok(views::write_shard_set(
        dstd::String(root), {dstd::String(ia), dstd::String(ib)}));
    dstd::String out = env.dir() + "/merged";
    dftu_utils_test::ok(
        views::merge_shard_set(dstd::String(root), dstd::String(out)));
    dstd::String merged_idx = dstd::String(determine_index_path("x", out));

    dstd::fs::remove(a).discard();
    dstd::fs::remove(b).discard();

    dftracer::utils::trace::AggQuery sharded;
    sharded.config = config;
    sharded.quantiles = config.compute_percentiles;
    sharded.sources.resize(2);
    sharded.sources[0].index_path = dstd::String(ia.data(), ia.size());
    sharded.sources[0].files.push_back(dstd::String(a.data(), a.size()));
    sharded.sources[1].index_path = dstd::String(ib.data(), ib.size());
    sharded.sources[1].files.push_back(dstd::String(b.data(), b.size()));
    const QueryOut expected = run_query(sharded);
    REQUIRE(expected.chunks_reread == 0);
    REQUIRE(expected.rows.size() == 2);

    dftracer::utils::trace::AggQuery one;
    one.config = config;
    one.quantiles = config.compute_percentiles;
    one.sources.resize(1);
    one.sources[0].index_path =
        dstd::String(merged_idx.data(), merged_idx.size());
    one.sources[0].files.push_back(dstd::String(a.data(), a.size()));
    one.sources[0].files.push_back(dstd::String(b.data(), b.size()));
    const QueryOut got = run_query(one);
    CHECK(got.chunks_reread == 0);
    CHECK(got.rows.size() == expected.rows.size());
    for (const auto& [key, value] : expected.rows) {
        const auto it = got.rows.find(key);
        REQUIRE_MESSAGE(it != got.rows.end(), key);
        CHECK_MESSAGE(it->second.count == value.count, key);
        CHECK_MESSAGE(it->second.sum == value.sum, key);
        if (config.compute_percentiles) {
            CHECK_MESSAGE(value.p90 > 0, key);
            CHECK_MESSAGE(it->second.p50 == value.p50, key);
            CHECK_MESSAGE(it->second.p90 == value.p90, key);
        }
    }
}

}  // namespace

TEST_SUITE("ShardedView") {
    TEST_CASE("aggregate over shards equals a single multi-index View") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String a = make_x_trace(env, "a", 30, 20);
        dstd::String b = make_x_trace(env, "b", 10, 40);
        dstd::String ia = dstd::String(determine_index_path(a, ""));
        dstd::String ib = dstd::String(determine_index_path(b, ""));
        build_shard_index(a);
        build_shard_index(b);

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()})
                .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "total"}});
        };

        auto expect = canon(dftu_utils_test::ok(
            configure(View::from_files({{dstd::String(a), dstd::String(ia)},
                                        {dstd::String(b), dstd::String(ib)}}))
                .collect()
                .get()));
        REQUIRE(expect.size() == 2);  // POSIX + STDIO

        auto got =
            canon(dftu_utils_test::ok(views::ShardedView::from_shard_dirs(
                                          {dstd::String(ia), dstd::String(ib)})
                                          .aggregate(configure)
                                          .get()));
        CHECK(got == expect);
    }

    TEST_CASE("counter aggregation over shards merges like one view") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String a = create_counter_file(env, "ca", {40, 40});
        dstd::String b = create_counter_file(env, "cb", {80, 80});
        dstd::String ia = dstd::String(determine_index_path(a, ""));
        dstd::String ib = dstd::String(determine_index_path(b, ""));
        build_shard_index(a);
        build_shard_index(b);

        auto configure = [](View v) -> View {
            return v.phase(Phase::Counters)
                .group_by({GroupKey::name()})
                .agg_numeric_args();
        };

        StringSink single;
        dftu_utils_test::ok(
            configure(View::from_files({{dstd::String(a), dstd::String(ia)},
                                        {dstd::String(b), dstd::String(ib)}}))
                .sink_counters(single)
                .get());

        StringSink sharded;
        dftu_utils_test::ok(views::ShardedView::from_shard_dirs(
                                {dstd::String(ia), dstd::String(ib)})
                                .aggregate_counters(configure, sharded)
                                .get());

        auto sl = single.lines();
        auto ml = sharded.lines();
        std::sort(sl.begin(), sl.end());
        std::sort(ml.begin(), ml.end());
        CHECK(ml == sl);
        REQUIRE(ml.size() == 1);
        CHECK(ml[0].find(R"("user_pct":60)") != dstd::String::npos);
    }

    TEST_CASE("from_manifest discovers shards and aggregates identically") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String a = make_x_trace(env, "ma", 30, 20);
        dstd::String b = make_x_trace(env, "mb", 10, 40);
        dstd::String ia = dstd::String(determine_index_path(a, ""));
        dstd::String ib = dstd::String(determine_index_path(b, ""));
        build_shard_index(a);
        build_shard_index(b);

        index::store::IndexShardManifest manifest;
        manifest.format_version =
            static_cast<std::uint32_t>(DFTRACER_UTILS_INDEX_FORMAT_VERSION);
        manifest.shards.push_back({dstd::String(dstd::StringView(ia)), 1, 0});
        manifest.shards.push_back({dstd::String(dstd::StringView(ib)), 1, 0});
        dstd::String root = env.dir() + "/set";
        dftu_utils_test::ok(index::store::write_shard_manifest(root, manifest));

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()}).agg({{AggOp::Count, "", "n"}});
        };

        auto expect = canon(dftu_utils_test::ok(
            configure(View::from_files({{dstd::String(a), dstd::String(ia)},
                                        {dstd::String(b), dstd::String(ib)}}))
                .collect()
                .get()));
        auto got = canon(dftu_utils_test::ok(
            dftu_utils_test::ok(
                views::ShardedView::from_manifest(dstd::String(root)))
                .aggregate(configure)
                .get()));
        CHECK(got == expect);
    }

    // The tier fast path answers from each shard's pre-folded aggregation
    // index, never reading the traces. Deleting the traces after the indexes
    // are built must leave the aggregate unchanged - proof no scan happened.
    TEST_CASE("aggregate serves from the tier with the traces deleted") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String a = make_x_trace(env, "ta", 30, 20);
        dstd::String b = make_x_trace(env, "tb", 10, 40);
        dstd::String ia = dstd::String(determine_index_path(a, ""));
        dstd::String ib = dstd::String(determine_index_path(b, ""));
        build_shard_index(a);
        build_shard_index(b);

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()})
                .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "total"}});
        };
        auto sharded = views::ShardedView::from_shard_dirs(
            {dstd::String(ia), dstd::String(ib)});
        auto with_traces =
            canon(dftu_utils_test::ok(sharded.aggregate(configure).get()));
        REQUIRE(with_traces.size() == 2);

        dstd::fs::remove(a).discard();
        dstd::fs::remove(b).discard();
        auto without_traces =
            canon(dftu_utils_test::ok(sharded.aggregate(configure).get()));
        CHECK(without_traces == with_traces);
    }

    TEST_CASE("write_shard_set catalogs shards into a readable manifest") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String a = make_x_trace(env, "wa", 30, 20);
        dstd::String b = make_x_trace(env, "wb", 10, 40);
        dstd::String ia = dstd::String(determine_index_path(a, ""));
        dstd::String ib = dstd::String(determine_index_path(b, ""));
        build_shard_index(a);
        build_shard_index(b);

        dstd::String root = env.dir() + "/set";
        dftu_utils_test::ok(views::write_shard_set(
            dstd::String(root), {dstd::String(ia), dstd::String(ib)}));

        auto manifest =
            dftu_utils_test::ok(index::store::read_shard_manifest(root));
        REQUIRE(manifest.has_value());
        CHECK(manifest->format_version ==
              static_cast<std::uint32_t>(DFTRACER_UTILS_INDEX_FORMAT_VERSION));
        REQUIRE(manifest->shards.size() == 2);
        for (const auto& s : manifest->shards) CHECK(s.file_count == 1);

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()}).agg({{AggOp::Count, "", "n"}});
        };
        auto expect = canon(dftu_utils_test::ok(
            configure(View::from_files({{dstd::String(a), dstd::String(ia)},
                                        {dstd::String(b), dstd::String(ib)}}))
                .collect()
                .get()));
        auto got = canon(dftu_utils_test::ok(
            dftu_utils_test::ok(
                views::ShardedView::from_manifest(dstd::String(root)))
                .aggregate(configure)
                .get()));
        CHECK(got == expect);
    }

    // Read-only proof: with the shard index directories chmod'd to read-only
    // (as an NFS export or a published immutable index would be), the query
    // still succeeds - so no write, lock, or rebuild is attempted.
    TEST_CASE("aggregate works against read-only shard directories") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String a = make_x_trace(env, "roa", 30, 20);
        dstd::String b = make_x_trace(env, "rob", 10, 40);
        dstd::String ia = dstd::String(determine_index_path(a, ""));
        dstd::String ib = dstd::String(determine_index_path(b, ""));
        build_shard_index(a);
        build_shard_index(b);

        REQUIRE(dstd::fs::set_permissions(ia, 0550).has_value());
        REQUIRE(dstd::fs::set_permissions(ib, 0550).has_value());

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()}).agg({{AggOp::Count, "", "n"}});
        };
        auto got =
            canon(dftu_utils_test::ok(views::ShardedView::from_shard_dirs(
                                          {dstd::String(ia), dstd::String(ib)})
                                          .aggregate(configure)
                                          .get()));

        // Restore write perms so the environment can clean up.
        REQUIRE(dstd::fs::set_permissions(ia, 0750).has_value());
        REQUIRE(dstd::fs::set_permissions(ib, 0750).has_value());

        CHECK(got.size() == 2);
    }

    // A shard stores each trace's absolute path, so a shard index moved away
    // from its traces still resolves them. SetUnion is not in the tier, so it
    // forces the scan (not the tier fast path), which needs the traces.
    TEST_CASE("a relocated shard finds its traces via the registry path") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String a = make_x_trace(env, "rel", 30, 20);
        dstd::String ia = dstd::String(determine_index_path(a, ""));
        build_shard_index(a);

        // Copy the index into a directory that holds no traces of its own. Copy
        // rather than move so the cached read-only handle stays valid.
        dstd::String movedir = env.dir() + "/relocated";
        REQUIRE(dstd::fs::create_directories(movedir).has_value());
        dstd::String moved = movedir + "/.dftindex";
        REQUIRE(dstd::fs::copy_tree(ia, moved, dstd::fs::CopyMode::FailIfExists)
                    .has_value());

        auto scan_cfg = [](View v) -> View {
            return v.group_by({GroupKey::cat()})
                .agg({{AggOp::SetUnion, "name", "names"}});
        };

        index::store::IndexShardManifest m;
        m.format_version =
            static_cast<std::uint32_t>(DFTRACER_UTILS_INDEX_FORMAT_VERSION);
        m.shards.push_back({dstd::String(dstd::StringView(moved)), 1, 0});
        dstd::String root = env.dir() + "/set";
        dftu_utils_test::ok(index::store::write_shard_manifest(root, m));
        auto got = dftu_utils_test::ok(
            dftu_utils_test::ok(
                views::ShardedView::from_manifest(dstd::String(root)))
                .aggregate(scan_cfg)
                .get());

        CHECK(got.num_rows() == 2);  // POSIX + STDIO, resolved via stored paths
    }

    // The merge moves each shard's data files into the root's own store, so the
    // root opens as one index and the shards are gone.
    TEST_CASE("consolidate_shard_set merges the shards into the root store") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String a = make_x_trace(env, "ga", 30, 20);
        dstd::String b = make_x_trace(env, "gb", 10, 40);
        dstd::String c = make_x_trace(env, "gc", 5, 5);
        const dstd::Vector<dstd::String> files{a, b, c};

        const dstd::String root = env.dir() + "/root/.dftindex";
        dstd::Vector<dstd::String> shards;
        for (std::size_t i = 0; i < files.size(); ++i) {
            shards.emplace_back(
                dstd::String(dftracer::utils::index::shard_root(root, i)));
            build_shard_index(files[i], {}, shards.back());
        }
        dstd::Vector<dstd::String> shard_dirs;
        for (const auto& shard : shards) shard_dirs.emplace_back(shard);
        dftu_utils_test::ok(
            views::write_shard_set(dstd::String(root), shard_dirs));
        auto manifest =
            dftu_utils_test::ok(index::store::read_shard_manifest(root));
        REQUIRE(manifest.has_value());
        REQUIRE(manifest->shards.size() == 3);
        CHECK(manifest->shards[0].path == "shards/0");

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()})
                .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "total"}});
        };
        const dstd::String single = env.dir() + "/single/.dftindex";
        for (const auto& f : files) build_shard_index(f, {}, single);
        auto expect = canon(dftu_utils_test::ok(
            configure(
                View::from_files({{dstd::String(a), dstd::String(single)},
                                  {dstd::String(b), dstd::String(single)},
                                  {dstd::String(c), dstd::String(single)}}))
                .collect()
                .get()));
        REQUIRE(expect.size() == 2);

        CHECK(dftu_utils_test::ok(
                  views::consolidate_shard_set(dstd::String(root))) == 3);
        CHECK_FALSE(dftu_utils_test::ok(index::store::read_shard_manifest(root))
                        .has_value());
        CHECK_FALSE(
            dstd::fs::exists(dftracer::utils::index::shards_dir(root)).value());
        CHECK(
            dstd::fs::exists(dftracer::utils::index::store_dir(root)).value());
        CHECK(indexed_files(root, files).size() == 3);

        auto got = canon(dftu_utils_test::ok(
            configure(View::from_files({{dstd::String(a), dstd::String(root)},
                                        {dstd::String(b), dstd::String(root)},
                                        {dstd::String(c), dstd::String(root)}}))
                .collect()
                .get()));
        CHECK(got == expect);
    }

    // A shard that cannot be listed keeps the manifest and the shards, and the
    // next call completes the merge without a rebuild.
    TEST_CASE("a failed merge keeps the shards for a retry") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String a = make_x_trace(env, "fa", 30, 20);
        dstd::String b = make_x_trace(env, "fb", 10, 40);
        const dstd::Vector<dstd::String> files{a, b};

        const dstd::String root = env.dir() + "/root/.dftindex";
        dstd::Vector<dstd::String> shards;
        for (std::size_t i = 0; i < files.size(); ++i) {
            shards.emplace_back(
                dstd::String(dftracer::utils::index::shard_root(root, i)));
            build_shard_index(files[i], {}, shards.back());
        }
        dstd::Vector<dstd::String> shard_dirs;
        for (const auto& shard : shards) shard_dirs.emplace_back(shard);
        dftu_utils_test::ok(
            views::write_shard_set(dstd::String(root), shard_dirs));

        // A regular file where the second shard's store belongs, so listing it
        // fails whatever user the merge runs as.
        const dstd::String store_b(
            dftracer::utils::index::store_dir(shards[1]));
        const dstd::String held = store_b + ".held";
        REQUIRE(dstd::fs::rename(store_b, held).has_value());
        {
            dftu_utils_test::FileOut ofs(store_b);
            ofs << "not a store";
        }

        CHECK_FALSE(
            views::consolidate_shard_set(dstd::String(root)).has_value());
        CHECK(dftu_utils_test::ok(index::store::read_shard_manifest(root))
                  .has_value());
        CHECK(dstd::fs::exists(shards[0]).value());
        CHECK(dstd::fs::exists(shards[1]).value());
        std::size_t moved = 0;
        for (const auto& entry : dftu_utils_test::list_dir(
                 dftracer::utils::index::store_dir(root))) {
            if (entry.path.view().extension() == ".dfs") ++moved;
        }
        CHECK(moved == 1);

        dstd::fs::remove(store_b).discard();
        REQUIRE(dstd::fs::rename(held, store_b).has_value());

        CHECK(dftu_utils_test::ok(
                  views::consolidate_shard_set(dstd::String(root))) == 2);
        CHECK_FALSE(dftu_utils_test::ok(index::store::read_shard_manifest(root))
                        .has_value());
        CHECK_FALSE(
            dstd::fs::exists(dftracer::utils::index::shards_dir(root)).value());
        CHECK(indexed_files(root, files).size() == 2);
        dftu_utils_test::expect_error(
            views::consolidate_shard_set(dstd::String(root)),
            dftracer::utils::common::ErrorCode::Io);
    }

    // No-rescan tier merge: transcode each shard's tier into one consolidated
    // index. Deleting the traces afterward proves it answers from the merged
    // tier, and the result must equal the sharded query.
    TEST_CASE("merge_shard_set builds a consolidated tier without traces") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String a = make_x_trace(env, "malpha", 30, 20);
        dstd::String b = make_x_trace(env, "mbeta", 10, 40);
        dstd::String ia = dstd::String(determine_index_path(a, ""));
        dstd::String ib = dstd::String(determine_index_path(b, ""));
        build_shard_index(a);
        build_shard_index(b);

        dstd::String root = env.dir() + "/set";
        dftu_utils_test::ok(views::write_shard_set(
            dstd::String(root), {dstd::String(ia), dstd::String(ib)}));

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()})
                .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "total"}});
        };
        auto sharded = canon(dftu_utils_test::ok(
            dftu_utils_test::ok(
                views::ShardedView::from_manifest(dstd::String(root)))
                .aggregate(configure)
                .get()));
        REQUIRE(sharded.size() == 2);

        dstd::String out = env.dir() + "/merged";
        std::size_t n = dftu_utils_test::ok(
            views::merge_shard_set(dstd::String(root), dstd::String(out)));
        CHECK(n == 2);

        // The tiers arrive in the new root; the shards stay where they are.
        CHECK(dftu_utils_test::ok(index::store::read_shard_manifest(root))
                  .has_value());
        CHECK_FALSE(dftu_utils_test::ok(index::store::read_shard_manifest(out))
                        .has_value());

        dstd::fs::remove(a).discard();
        dstd::fs::remove(b).discard();
        const dstd::String out_idx =
            dstd::String(determine_index_path("x", out));
        auto merged = canon(dftu_utils_test::ok(
            configure(
                View::from_files({{dstd::String(a), dstd::String(out_idx)},
                                  {dstd::String(b), dstd::String(out_idx)}}))
                .collect()
                .get()));
        CHECK(merged == sharded);
    }

    // The partials must reach the merged index, or a query reads the traces.
    TEST_CASE("merge_shard_set copies the aggregation partials") {
        check_merged_partials("p", {});
    }

    // A shard built with another sketch accuracy keeps its quantiles.
    TEST_CASE("merge_shard_set keeps quantiles of a non-default accuracy") {
        index::schemas::dft::agg::AggregationConfig config;
        config.compute_percentiles = true;
        config.sketch_accuracy = 0.05;
        check_merged_partials("q", config);
    }

    TEST_CASE("from_manifest is an Io error when the manifest is absent") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dftu_utils_test::expect_error(views::ShardedView::from_manifest(
                                          dstd::String(env.dir() + "/nope")),
                                      dftracer::utils::common::ErrorCode::Io);
    }

    TEST_CASE("from_manifest rejects a manifest with no or another version") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        dstd::String root = env.dir() + "/set";
        REQUIRE(dstd::fs::create_directories(root).has_value());

        {
            dftu_utils_test::FileOut ofs(root + "/shards.json");
            ofs << R"({"shards":[]})";
        }
        dftu_utils_test::expect_error(
            views::ShardedView::from_manifest(dstd::String(root)),
            dftracer::utils::common::ErrorCode::InvalidArgument);

        index::store::IndexShardManifest other;
        other.format_version =
            static_cast<std::uint32_t>(DFTRACER_UTILS_INDEX_FORMAT_VERSION) + 1;
        dftu_utils_test::ok(index::store::write_shard_manifest(root, other));
        dftu_utils_test::expect_error(
            views::ShardedView::from_manifest(dstd::String(root)),
            dftracer::utils::common::ErrorCode::InvalidArgument);

        index::store::IndexShardManifest current;
        current.format_version =
            static_cast<std::uint32_t>(DFTRACER_UTILS_INDEX_FORMAT_VERSION);
        dftu_utils_test::ok(index::store::write_shard_manifest(root, current));
        CHECK(
            views::ShardedView::from_manifest(dstd::String(root)).has_value());
    }
}
