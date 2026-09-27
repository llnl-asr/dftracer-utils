// The query cache: rollups and materialized views in .dftindex-cache beside
// the index, capped with LRU eviction, disposable, and keyed by record schema.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/cache/rollup_store.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace {

namespace rdb = dftracer::utils::index::store;
namespace scan = dftracer::utils::trace::views::detail::scan;

std::string cache_of(const std::string& gz) {
    return (fs::path(gz).parent_path() / ".dftindex-cache").string();
}

std::string rollups_of(const std::string& gz) {
    return (fs::path(cache_of(gz)) / "rollups").string();
}

// Signatures in the rollup store, from its usage records (0x02 | sig).
std::vector<std::string> rollup_sigs(const std::string& gz) {
    std::vector<std::string> out;
    auto db = dftracer::utils::index::cache::open_rollup_db(
        rollups_of(gz), rdb::RocksDatabase::OpenMode::ReadOnly);
    auto it = db->new_iterator(rdb::cf::ROLLUP);
    const char tag = '\x02';
    for (it->Seek(std::string_view(&tag, 1)); it->Valid(); it->Next()) {
        const std::string_view k(it->key().data(), it->key().size());
        if (k.empty() || k[0] != tag) break;
        out.emplace_back(k);
    }
    return out;
}

std::uint64_t rollup_bytes(const std::string& gz) {
    std::uint64_t total = 0;
    auto db = dftracer::utils::index::cache::open_rollup_db(
        rollups_of(gz), rdb::RocksDatabase::OpenMode::ReadOnly);
    auto it = db->new_iterator(rdb::cf::ROLLUP);
    const char tag = '\x02';
    for (it->Seek(std::string_view(&tag, 1)); it->Valid(); it->Next()) {
        const std::string_view k(it->key().data(), it->key().size());
        if (k.empty() || k[0] != tag) break;
        std::uint64_t b = 0;
        for (int i = 0; i < 8; ++i)
            b = (b << 8) | static_cast<unsigned char>(it->value()[i]);
        total += b;
    }
    return total;
}

View by_cat(const std::string& gz, const std::string& query) {
    return View::from_file(gz, determine_index_path(gz, ""))
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
    View::from_file(gz, determine_index_path(gz, "")).sink_json(warm).get();
    return gz;
}

}  // namespace

TEST_SUITE("QueryCache") {
    TEST_CASE("rollups go beside the index, not into it") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env);
        by_cat(gz, "dur >= 0").materialize().get();
        CHECK(fs::exists(fs::path(rollups_of(gz)) / "CURRENT"));
        CHECK(rollup_sigs(gz).size() == 1);
        CHECK_FALSE(
            fs::exists(fs::path(determine_index_path(gz, "")) / "cache"));
        rdb::IndexDatabase idx(determine_index_path(gz, ""),
                               rdb::IndexOpenMode::ReadOnly);
        auto it = idx.db()->new_iterator(rdb::cf::ROLLUP);
        it->SeekToFirst();
        CHECK_FALSE(it->Valid());
    }

    TEST_CASE("deleting the cache changes no result") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env);
        const std::string idx = determine_index_path(gz, "");
        const auto agg = canon(by_cat(gz, "dur >= 0").collect().get());
        StringSink rows;
        View::from_file(gz, idx)
            .duql(R"(name == "read")")
            .sink_json(rows)
            .get();

        by_cat(gz, "dur >= 0").materialize().get();
        View::from_file(gz, idx).duql(R"(name == "read")").materialize().get();
        REQUIRE(fs::exists(fs::path(cache_of(gz)) / "views"));
        CHECK(canon(by_cat(gz, "dur >= 0").collect().get()) == agg);

        dftracer::utils::index::store::RocksDBManager::instance().reset(
            rollups_of(gz));
        fs::remove_all(cache_of(gz));
        CHECK(canon(by_cat(gz, "dur >= 0").collect().get()) == agg);
        StringSink again;
        const auto st = View::from_file(gz, idx)
                            .duql(R"(name == "read")")
                            .sink_json(again)
                            .get();
        CHECK(again.lines() == rows.lines());
        CHECK(st.events_scanned == 50);
    }

    TEST_CASE("a busy rollup store does not fail the query") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env);
        const auto want = canon(by_cat(gz, "dur >= 0").collect().get());
        fs::create_directories(rollups_of(gz));
        // Holds the store's lock outside the manager, as another process would.
        rdb::RocksDatabase busy(rollups_of(gz),
                                rdb::RocksDatabase::OpenMode::ReadWrite);
        REQUIRE(busy.is_open());
        CHECK_NOTHROW(by_cat(gz, "dur >= 0").materialize().get());
        CHECK(canon(by_cat(gz, "dur >= 0").collect().get()) == want);
    }

    TEST_CASE("the rollup store evicts the least recently used rollup") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env);
        const std::string idx = determine_index_path(gz, "");
        auto plan = [&](const std::string& q) {
            return scan::agg(
                scan::group_by(scan::filter(scan::from_file(gz, idx),
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
        ::setenv("DFTRACER_CACHE_MAX_BYTES", std::to_string(two).c_str(), 1);
        CHECK(materialize("dur >= 0") == a);
        materialize("dur >= 2");
        const auto left = rollup_sigs(gz);
        ::unsetenv("DFTRACER_CACHE_MAX_BYTES");
        REQUIRE(left.size() == 2);
        CHECK(std::find(left.begin(), left.end(), one[0]) != left.end());
        CHECK(rollup_bytes(gz) <= two);
    }

    TEST_CASE(
        "a rollup made under one record schema is not reused by another") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env);
        const std::string idx = determine_index_path(gz, "");
        auto view = [&](bool generic) {
            View v = View::from_file(gz, idx);
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
        const std::string idx = determine_index_path(gz, "");
        ::setenv("DFTRACER_CACHE_MAX_BYTES", "1", 1);
        View::from_file(gz, idx).duql(R"(name == "read")").materialize().get();
        ::unsetenv("DFTRACER_CACHE_MAX_BYTES");
        std::vector<std::string> views;
        std::error_code ec;
        for (fs::directory_iterator it(fs::path(cache_of(gz)) / "views", ec),
             end;
             !ec && it != end; it.increment(ec))
            views.push_back(it->path().string());
        CHECK(views.empty());
        StringSink rows;
        const auto st = View::from_file(gz, idx)
                            .duql(R"(name == "read")")
                            .sink_json(rows)
                            .get();
        CHECK(rows.lines().size() == 30);
        CHECK(st.events_scanned == 50);
    }
}
