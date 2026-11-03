// SPDX-License-Identifier: MIT
// The query cache: rollups and materialized views inside the index root, in
// its rollups/, lookups/ and views/, capped with LRU eviction, disposable, and
// keyed by record schema.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/cache/rollup_store.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace {

namespace scan = dftracer::utils::trace::views::detail::scan;

std::string cache_of(const std::string& gz) {
    return std::string(determine_index_path(gz, ""));
}

std::string rollups_of(const std::string& gz) {
    return std::string(dftracer::utils::index::rollups_dir(cache_of(gz)));
}

std::string views_of(const std::string& gz) {
    return std::string(dftracer::utils::index::views_dir(cache_of(gz)));
}

// Signatures in the rollup store, from its usage records.
std::vector<std::uint64_t> rollup_sigs(const std::string& gz) {
    std::vector<std::uint64_t> out;
    auto db = dftracer::utils::index::cache::open_rollup_db(
        rollups_of(gz),
        dftracer::utils::index::cache::CacheStore::Mode::ReadOnly);
    for (const auto& u : db->usage()) out.push_back(u.sig);
    return out;
}

std::uint64_t rollup_bytes(const std::string& gz) {
    std::uint64_t total = 0;
    auto db = dftracer::utils::index::cache::open_rollup_db(
        rollups_of(gz),
        dftracer::utils::index::cache::CacheStore::Mode::ReadOnly);
    for (const auto& u : db->usage()) total += u.bytes;
    return total;
}

View by_cat(const std::string& gz, const std::string& query) {
    return View::from_file(
               dstd::String(gz),
               dstd::String(std::string(determine_index_path(gz, ""))))
        .duql(query)
        .group_by({GroupKey::cat()})
        .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "total"}});
}

std::vector<std::string> canon(const dataframe::DataFrame& df) {
    std::vector<std::string> rows;
    for (std::int64_t r = 0; r < df.num_rows(); ++r)
        rows.push_back(bstr(df, r, "cat") + "|" +
                       std::to_string(bnum(df, r, "n")) + "|" +
                       std::to_string(bnum(df, r, "total")));
    std::sort(rows.begin(), rows.end());
    return rows;
}

std::string indexed_trace(TestEnvironment& env) {
    std::string gz = create_mixed_trace(env, 30, 20);
    StringSink warm;
    View::from_file(dstd::String(gz),
                    dstd::String(std::string(determine_index_path(gz, ""))))
        .sink_json(warm)
        .get();
    return gz;
}

}  // namespace

TEST_SUITE("QueryCache") {
    TEST_CASE("rollups go in the index root, not in its store") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env);
        by_cat(gz, "dur >= 0").materialize().get();
        CHECK(
            dftracer::utils::index::cache::CacheStore::exists(rollups_of(gz)));
        CHECK(rollup_sigs(gz).size() == 1);
        CHECK_FALSE(dstd::fs::exists(dstd::Path(cache_of(gz)).view().parent() /
                                     ".dftindex-cache")
                        .value());
        auto store = dftracer::utils::store::Store::open(
            (dstd::Path(std::string(determine_index_path(gz, ""))) / "store")
                .str());
        REQUIRE(store.has_value());
        auto snapshot = store->snapshot();
        REQUIRE(snapshot.has_value());
        auto declared = snapshot->namespaces();
        REQUIRE(declared.has_value());
        bool rollup_in_index = false;
        for (const auto& decl : *declared) {
            if (dstd::StringView(decl.name.data(), decl.name.size())
                    .find("rollup") != dstd::StringView::npos)
                rollup_in_index = true;
        }
        CHECK_FALSE(rollup_in_index);
    }

    TEST_CASE("deleting the cache changes no result") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env);
        const std::string idx = std::string(determine_index_path(gz, ""));
        const auto agg = canon(by_cat(gz, "dur >= 0").collect().get());
        StringSink rows;
        View::from_file(dstd::String(gz), dstd::String(idx))
            .duql(R"(name == "read")")
            .sink_json(rows)
            .get();

        by_cat(gz, "dur >= 0").materialize().get();
        View::from_file(dstd::String(gz), dstd::String(idx))
            .duql(R"(name == "read")")
            .materialize()
            .get();
        REQUIRE(dstd::fs::exists(dstd::Path(views_of(gz))).value());
        CHECK(canon(by_cat(gz, "dur >= 0").collect().get()) == agg);

        // Both caches are disposable. The index itself is not part of them.
        dstd::fs::remove_all(rollups_of(gz)).has_value();
        (void)dstd::fs::remove_all(views_of(gz));
        CHECK(canon(by_cat(gz, "dur >= 0").collect().get()) == agg);
        StringSink again;
        const auto st = View::from_file(dstd::String(gz), dstd::String(idx))
                            .duql(R"(name == "read")")
                            .sink_json(again)
                            .get();
        CHECK(again.lines() == rows.lines());
        CHECK(st.events_scanned == 50);
    }

    TEST_CASE("a root holding only caches is not mistaken for an index") {
        TestEnvironment env(200);
        const auto gz = create_mixed_trace(env, 30, 20);
        const std::string idx = std::string(determine_index_path(gz, ""));
        by_cat(gz, "dur >= 0").materialize().get();
        const dstd::Path store = dstd::Path(idx) / "store";
        REQUIRE(dstd::fs::exists(store).value());

        // The caches outlive the index, so the root stays without its store.
        dstd::fs::remove_all(store).has_value();
        REQUIRE(dstd::fs::exists(dstd::Path(rollups_of(gz))).value());
        REQUIRE_FALSE(dstd::fs::exists(store).value());

        StringSink rows;
        const auto st = View::from_file(dstd::String(gz), dstd::String(idx))
                            .duql(R"(name == "read")")
                            .sink_json(rows)
                            .get();
        CHECK(rows.lines().size() == 30);
        CHECK(st.events_scanned == 50);
        CHECK(dstd::fs::exists(store).value());
    }

    TEST_CASE("a rollup store that cannot be opened does not fail the query") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env);
        const auto want = canon(by_cat(gz, "dur >= 0").collect().get());
        REQUIRE(dstd::fs::create_directories(cache_of(gz)).has_value());
        std::ofstream((rollups_of(gz)).c_str()) << "x";
        CHECK_NOTHROW(by_cat(gz, "dur >= 0").materialize().get());
        CHECK(canon(by_cat(gz, "dur >= 0").collect().get()) == want);
    }

    TEST_CASE("the rollup store evicts the least recently used rollup") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env);
        const std::string idx = std::string(determine_index_path(gz, ""));
        auto plan = [&](const std::string& q) {
            return scan::agg(
                scan::group_by(scan::filter(scan::from_file(dstd::String(gz),
                                                            dstd::String(idx)),
                                            duql::parse_or_throw(q)),
                               {GroupKey::cat()}),
                {{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "total"}});
        };
        auto materialize = [&](const std::string& q) {
            return canon(
                run(scan::collect_frame(scan::materialize(plan(q), 0, 0))));
        };
        const auto a = materialize("dur >= 0");
        const auto one = rollup_sigs(gz);
        REQUIRE(one.size() == 1);
        materialize("dur >= 1");
        REQUIRE(rollup_sigs(gz).size() == 2);
        const std::uint64_t two = rollup_bytes(gz);

        // Room for two rollups of this shape; A is used again, so B goes.
        ::setenv("DFTRACER_UTILS_CACHE_MAX_BYTES", std::to_string(two).c_str(),
                 1);
        CHECK(materialize("dur >= 0") == a);
        materialize("dur >= 2");
        const auto left = rollup_sigs(gz);
        ::unsetenv("DFTRACER_UTILS_CACHE_MAX_BYTES");
        REQUIRE(left.size() == 2);
        CHECK(std::find(left.begin(), left.end(), one[0]) != left.end());
        CHECK(rollup_bytes(gz) <= two);
    }

    TEST_CASE(
        "a rollup made under one record schema is not reused by another") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env);
        const std::string idx = std::string(determine_index_path(gz, ""));
        auto view = [&](bool generic) {
            View v = View::from_file(dstd::String(gz), dstd::String(idx));
            if (generic) v = v.record_schema("generic");
            return v.group_by(std::vector<std::string>{"cat"},
                              {{dataframe::Agg::Count, "", "n"}});
        };
        view(false).materialize().get();
        REQUIRE(rollup_sigs(gz).size() == 1);
        view(true).materialize().get();
        CHECK(rollup_sigs(gz).size() == 2);
    }

    TEST_CASE("a view larger than the whole budget is not kept") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env);
        const std::string idx = std::string(determine_index_path(gz, ""));
        ::setenv("DFTRACER_UTILS_CACHE_MAX_BYTES", "1", 1);
        View::from_file(dstd::String(gz), dstd::String(idx))
            .duql(R"(name == "read")")
            .materialize()
            .get();
        ::unsetenv("DFTRACER_UTILS_CACHE_MAX_BYTES");
        std::vector<std::string> views;
        for (const auto& e :
             dftu_utils_test::list_dir(dstd::Path(views_of(gz))))
            views.push_back(std::string(e.path.str()));
        CHECK(views.empty());
        StringSink rows;
        const auto st = View::from_file(dstd::String(gz), dstd::String(idx))
                            .duql(R"(name == "read")")
                            .sink_json(rows)
                            .get();
        CHECK(rows.lines().size() == 30);
        CHECK(st.events_scanned == 50);
    }
}
