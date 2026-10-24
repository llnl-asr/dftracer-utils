#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/shard_manifest.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/views/sharded_view.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "test_view_common.h"
#include "testing_runtime.h"

using dftracer::utils::index::store::IndexDatabase;
using dftracer::utils::index::store::IndexShardManifest;
using dftracer::utils::index::store::read_shard_manifest;
using dftracer::utils::index::store::write_shard_manifest;
using dftracer::utils::trace::views::consolidate_shard_set;
using dftracer::utils::trace::views::merge_shard_set;
using dftracer::utils::trace::views::ShardedView;
using dftracer::utils::trace::views::write_shard_set;
using dftu_utils_test::run_coro;

namespace {

// A ph="X" trace in its own subdir, so each shard's sidecar index is distinct.
std::string make_x_trace(TestEnvironment& env, const std::string& tag,
                         int posix_n, int stdio_n) {
    std::string dir = env.get_dir() + "/" + tag;
    fs::create_directories(dir);
    std::string pfw = dir + "/" + tag + ".pfw";
    std::ofstream ofs(pfw);
    for (int i = 0; i < posix_n; ++i)
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i) << R"(,"args":{}})"
            << "\n";
    for (int i = 0; i < stdio_n; ++i)
        ofs << R"({"ph":"X","name":"fwrite","cat":"STDIO","pid":1,"tid":1,"ts":)"
            << (5000 + i * 100) << R"(,"dur":)" << (20 + i) << R"(,"args":{}})"
            << "\n";
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

namespace aggregators = dftracer::utils::index::schemas::dft::agg;

// Build a full sidecar index including the aggregation tier the way the
// aggregator does, so the chunk partials can answer without a scan.
void build_shard_index(const std::string& gz,
                       const aggregators::AggregationConfig& config = {}) {
    dftracer::utils::index::IndexerOptions options;
    options.aggregation = config;
    dftracer::utils::index::Indexer::open({gz}, options).build();
}

// Render a DataFrame canonically (String columns are the group keys, numeric
// columns the values) for order-independent comparison.
std::vector<std::string> canon(const dataframe::DataFrame& b) {
    std::vector<std::string> rows;
    for (std::int64_t i = 0; i < b.num_rows(); ++i) {
        std::string s;
        for (const auto& c : b.columns) {
            if (c.type() == dataframe::TypeId::String) {
                s += std::string(c.string_at(i)) + "|";
            } else {
                double v = (c.type() == dataframe::TypeId::Int64)
                               ? static_cast<double>(c.data<std::int64_t>()[i])
                           : (c.type() == dataframe::TypeId::Uint64)
                               ? static_cast<double>(c.data<std::uint64_t>()[i])
                               : c.data<double>()[i];
                s += std::to_string(v) + "|";
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
    std::map<std::string, Row> rows;
    std::size_t chunks_reread = 0;
};

QueryOut run_query(const dftracer::utils::trace::AggQuery& q) {
    QueryOut out;
    std::string error;
    run_coro(
        [&](dftracer::utils::CoroScope&)
            -> dftracer::utils::coro::CoroTask<void> {
            auto r = co_await dftracer::utils::trace::run_agg_query(q);
            if (!r) {
                error = r.error().format();
                co_return;
            }
            out.chunks_reread = r->stats.chunks_reread;
            for (const auto& [k, p] : r->rows)
                out.rows[std::string(r->text(k.cat)) + "|" +
                         std::string(r->text(k.name))] = {
                    p.count, p.duration.sum, p.duration_q.quantile(0.5),
                    p.duration_q.quantile(0.9)};
            co_return;
        },
        "sharded-view-agg-query");
    REQUIRE_MESSAGE(error.empty(), error);
    return out;
}

void check_merged_partials(const std::string& tag,
                           const aggregators::AggregationConfig& config) {
    TestEnvironment env(200);
    REQUIRE(env.is_valid());
    std::string a = make_x_trace(env, tag + "alpha", 30, 20);
    std::string b = make_x_trace(env, tag + "beta", 10, 40);
    std::string ia = determine_index_path(a, "");
    std::string ib = determine_index_path(b, "");
    build_shard_index(a, config);
    build_shard_index(b, config);

    std::string root = env.get_dir() + "/set";
    write_shard_set(root, {ia, ib});
    std::string out = env.get_dir() + "/merged";
    merge_shard_set(root, out);
    std::string merged_idx = determine_index_path("x", out);

    fs::remove(a);
    fs::remove(b);

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
        std::string a = make_x_trace(env, "a", 30, 20);
        std::string b = make_x_trace(env, "b", 10, 40);
        std::string ia = determine_index_path(a, "");
        std::string ib = determine_index_path(b, "");
        build_shard_index(a);
        build_shard_index(b);

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()})
                .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "total"}});
        };

        auto expect = canon(
            configure(View::from_files({{a, ia}, {b, ib}})).collect().get());
        REQUIRE(expect.size() == 2);  // POSIX + STDIO

        auto got = canon(
            ShardedView::from_shard_dirs({ia, ib}).aggregate(configure).get());
        CHECK(got == expect);
    }

    TEST_CASE("counter aggregation over shards merges like one view") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string a = create_counter_file(env, "ca", {40, 40});
        std::string b = create_counter_file(env, "cb", {80, 80});
        std::string ia = determine_index_path(a, "");
        std::string ib = determine_index_path(b, "");
        build_shard_index(a);
        build_shard_index(b);

        auto configure = [](View v) -> View {
            return v.phase(Phase::Counters)
                .group_by({GroupKey::name()})
                .agg_numeric_args();
        };

        StringSink single;
        configure(View::from_files({{a, ia}, {b, ib}}))
            .sink_counters(single)
            .get();

        StringSink sharded;
        ShardedView::from_shard_dirs({ia, ib})
            .aggregate_counters(configure, sharded)
            .get();

        auto sl = single.lines();
        auto ml = sharded.lines();
        std::sort(sl.begin(), sl.end());
        std::sort(ml.begin(), ml.end());
        CHECK(ml == sl);
        REQUIRE(ml.size() == 1);
        CHECK(ml[0].find(R"("user_pct":60)") != std::string::npos);
    }

    TEST_CASE("from_manifest discovers shards and aggregates identically") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string a = make_x_trace(env, "ma", 30, 20);
        std::string b = make_x_trace(env, "mb", 10, 40);
        std::string ia = determine_index_path(a, "");
        std::string ib = determine_index_path(b, "");
        build_shard_index(a);
        build_shard_index(b);

        IndexShardManifest manifest;
        manifest.format_version = IndexDatabase::FORMAT_VERSION;
        manifest.shards.push_back({ia, 0, 0, 1, 0});
        manifest.shards.push_back({ib, 0, 0, 1, 0});
        std::string root = env.get_dir() + "/set";
        write_shard_manifest(root, manifest);

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()}).agg({{AggOp::Count, "", "n"}});
        };

        auto expect = canon(
            configure(View::from_files({{a, ia}, {b, ib}})).collect().get());
        auto got =
            canon(ShardedView::from_manifest(root).aggregate(configure).get());
        CHECK(got == expect);
    }

    // The tier fast path answers from each shard's pre-folded aggregation
    // index, never reading the traces. Deleting the traces after the indexes
    // are built must leave the aggregate unchanged - proof no scan happened.
    TEST_CASE("aggregate serves from the tier with the traces deleted") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string a = make_x_trace(env, "ta", 30, 20);
        std::string b = make_x_trace(env, "tb", 10, 40);
        std::string ia = determine_index_path(a, "");
        std::string ib = determine_index_path(b, "");
        build_shard_index(a);
        build_shard_index(b);

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()})
                .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "total"}});
        };
        auto sharded = ShardedView::from_shard_dirs({ia, ib});
        auto with_traces = canon(sharded.aggregate(configure).get());
        REQUIRE(with_traces.size() == 2);

        fs::remove(a);
        fs::remove(b);
        auto without_traces = canon(sharded.aggregate(configure).get());
        CHECK(without_traces == with_traces);
    }

    TEST_CASE("write_shard_set catalogs shards into a readable manifest") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string a = make_x_trace(env, "wa", 30, 20);
        std::string b = make_x_trace(env, "wb", 10, 40);
        std::string ia = determine_index_path(a, "");
        std::string ib = determine_index_path(b, "");
        build_shard_index(a);
        build_shard_index(b);

        std::string root = env.get_dir() + "/set";
        write_shard_set(root, {ia, ib});

        auto manifest = read_shard_manifest(root);
        REQUIRE(manifest.has_value());
        CHECK(manifest->format_version == IndexDatabase::FORMAT_VERSION);
        REQUIRE(manifest->shards.size() == 2);
        for (const auto& s : manifest->shards) CHECK(s.num_files == 1);

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()}).agg({{AggOp::Count, "", "n"}});
        };
        auto expect = canon(
            configure(View::from_files({{a, ia}, {b, ib}})).collect().get());
        auto got =
            canon(ShardedView::from_manifest(root).aggregate(configure).get());
        CHECK(got == expect);
    }

    // Read-only proof: with the shard index directories chmod'd to read-only
    // (as an NFS export or a published immutable index would be), the query
    // still succeeds - so no write, lock, or rebuild is attempted.
    TEST_CASE("aggregate works against read-only shard directories") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string a = make_x_trace(env, "roa", 30, 20);
        std::string b = make_x_trace(env, "rob", 10, 40);
        std::string ia = determine_index_path(a, "");
        std::string ib = determine_index_path(b, "");
        build_shard_index(a);
        build_shard_index(b);

        std::error_code ec;
        const auto ro = fs::perms::owner_read | fs::perms::owner_exec |
                        fs::perms::group_read | fs::perms::group_exec;
        fs::permissions(ia, ro, fs::perm_options::replace, ec);
        fs::permissions(ib, ro, fs::perm_options::replace, ec);

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()}).agg({{AggOp::Count, "", "n"}});
        };
        auto got = canon(
            ShardedView::from_shard_dirs({ia, ib}).aggregate(configure).get());

        // Restore write perms so the environment can clean up.
        fs::permissions(ia, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::permissions(ib, fs::perms::owner_all, fs::perm_options::add, ec);

        CHECK(got.size() == 2);
    }

    // The registry stores each trace's absolute path, so a shard index moved
    // away from its traces still resolves them. SetUnion is not in the tier, so
    // it forces the scan (not the tier fast path), which needs the traces.
    TEST_CASE("a relocated shard finds its traces via the registry path") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string a = make_x_trace(env, "rel", 30, 20);
        std::string ia = determine_index_path(a, "");
        build_shard_index(a);

        // Copy the index into a directory that holds no traces of its own. Copy
        // rather than move so the cached read-only handle stays valid.
        std::string movedir = env.get_dir() + "/relocated";
        fs::create_directories(movedir);
        std::string moved = movedir + "/.dftindex";
        fs::copy(ia, moved, fs::copy_options::recursive);

        auto scan_cfg = [](View v) -> View {
            return v.group_by({GroupKey::cat()})
                .agg({{AggOp::SetUnion, "name", "names"}});
        };

        IndexShardManifest m;
        m.format_version = IndexDatabase::FORMAT_VERSION;
        m.shards.push_back({moved, 0, 0, 1, 0});
        std::string root = env.get_dir() + "/set";
        write_shard_manifest(root, m);
        auto got = ShardedView::from_manifest(root).aggregate(scan_cfg).get();

        CHECK(got.num_rows() == 2);  // POSIX + STDIO, resolved via stored paths
    }

    // Consolidation rebuilds the set into one unified index+tier; a query over
    // it opens a single shard and answers identically to the N-shard set.
    TEST_CASE("consolidate_shard_set builds one unified tier index") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string a = make_x_trace(env, "alpha", 30, 20);
        std::string b = make_x_trace(env, "beta", 10, 40);
        std::string ia = determine_index_path(a, "");
        std::string ib = determine_index_path(b, "");
        build_shard_index(a);
        build_shard_index(b);

        std::string root = env.get_dir() + "/set";
        write_shard_set(root, {ia, ib});

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()}).agg({{AggOp::Count, "", "n"}});
        };
        auto sharded =
            canon(ShardedView::from_manifest(root).aggregate(configure).get());
        REQUIRE(sharded.size() == 2);

        std::string out = env.get_dir() + "/unified";
        std::size_t n = 0;
        run_coro(
            [&](dftracer::utils::CoroScope& scope)
                -> dftracer::utils::coro::CoroTask<void> {
                n = co_await consolidate_shard_set(&scope, root, out);
                co_return;
            },
            "sharded-view-test");
        CHECK(n == 2);

        auto m = read_shard_manifest(out);
        REQUIRE(m.has_value());
        CHECK(m->shards.size() == 1);
        CHECK(m->shards[0].num_files == 2);

        auto consolidated =
            canon(ShardedView::from_manifest(out).aggregate(configure).get());
        CHECK(consolidated == sharded);
    }

    // No-rescan tier merge: transcode each shard's tier into one consolidated
    // index. Deleting the traces afterward proves it answers from the merged
    // tier, and the result must equal the sharded query.
    TEST_CASE("merge_shard_set builds a consolidated tier without traces") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string a = make_x_trace(env, "malpha", 30, 20);
        std::string b = make_x_trace(env, "mbeta", 10, 40);
        std::string ia = determine_index_path(a, "");
        std::string ib = determine_index_path(b, "");
        build_shard_index(a);
        build_shard_index(b);

        std::string root = env.get_dir() + "/set";
        write_shard_set(root, {ia, ib});

        auto configure = [](View v) -> View {
            return v.group_by({GroupKey::cat()})
                .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "total"}});
        };
        auto sharded =
            canon(ShardedView::from_manifest(root).aggregate(configure).get());
        REQUIRE(sharded.size() == 2);

        std::string out = env.get_dir() + "/merged";
        std::size_t n = merge_shard_set(root, out);
        CHECK(n == 2);

        auto m = read_shard_manifest(out);
        REQUIRE(m.has_value());
        CHECK(m->shards.size() == 1);

        fs::remove(a);
        fs::remove(b);
        auto merged =
            canon(ShardedView::from_manifest(out).aggregate(configure).get());
        CHECK(merged == sharded);
    }

    // The partials must reach the merged index, or a query reads the traces.
    TEST_CASE("merge_shard_set copies the aggregation partials") {
        check_merged_partials("p", {});
    }

    // A shard built with another sketch accuracy keeps its quantiles.
    TEST_CASE("merge_shard_set keeps quantiles of a non-default accuracy") {
        aggregators::AggregationConfig config;
        config.compute_percentiles = true;
        config.sketch_accuracy = 0.05;
        check_merged_partials("q", config);
    }

    TEST_CASE("from_manifest throws when the manifest is absent") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        CHECK_THROWS(ShardedView::from_manifest(env.get_dir() + "/nope"));
    }

    TEST_CASE("from_manifest rejects a manifest with no or another version") {
        TestEnvironment env(200);
        REQUIRE(env.is_valid());
        std::string root = env.get_dir() + "/set";
        fs::create_directories(root);

        {
            std::ofstream ofs(root + "/shards.json", std::ios::trunc);
            ofs << R"({"shards":[]})";
        }
        CHECK_THROWS(ShardedView::from_manifest(root));

        IndexShardManifest other;
        other.format_version = IndexDatabase::FORMAT_VERSION + 1;
        write_shard_manifest(root, other);
        CHECK_THROWS(ShardedView::from_manifest(root));

        IndexShardManifest current;
        current.format_version = IndexDatabase::FORMAT_VERSION;
        write_shard_manifest(root, current);
        CHECK_NOTHROW(ShardedView::from_manifest(root));
    }
}
