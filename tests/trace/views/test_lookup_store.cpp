// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/trace/index/cache/lookup_store.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <memory/fresh_process.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

namespace {

using namespace dftracer::utils::trace;

dstd::String indexed_trace(TestEnvironment& env, int posix_n) {
    const dstd::String gz = create_mixed_trace(env, posix_n, 20);
    dftu_utils_test::ok(
        dftu_utils_test::ok(dftracer::utils::trace::index::Indexer::open(
                                {dstd::String(dstd::StringView(gz))}))
            .build());
    return gz;
}

auto plan_of(const dstd::String& gz) {
    auto p = views::detail::scan::from_file(
        dstd::String(gz),
        dstd::String(dstd::String(determine_index_path(gz, ""))));
    dftracer::utils::trace::views::detail::plan_record_schema(*p);
    return p;
}

dstd::SharedPtr<trace::index::cache::CacheStore> store_in(
    TestEnvironment& env) {
    return trace::index::cache::open_lookup_db(
        env.dir() + "/lookups",
        trace::index::cache::CacheStore::Mode::ReadWrite);
}

dstd::String lookup_trace(TestEnvironment& env) {
    const dstd::String plain = env.dir() + "/side.ndjson";
    {
        dftu_utils_test::FileOut o(plain);
        o << R"({"type":"run","run":1,"app":"laghos"})" << "\n";
        o << R"({"type":"run","run":2,"app":"amg"})" << "\n";
        for (int i = 0; i < 4; ++i)
            o << R"({"type":"ev","i":)" << i << R"(,"run":)" << i % 2 + 1
              << "}\n";
    }
    const dstd::String gz = plain + ".gz";
    dftu_utils_test::compress_file_to_gzip(plain, gz);
    dstd::fs::remove(plain).discard();
    dftu_utils_test::ok(
        dftu_utils_test::ok(dftracer::utils::trace::index::Indexer::open(
                                {dstd::String(dstd::StringView(gz))}))
            .build());
    return gz;
}

dstd::String lookup_query() {
    return "let runs = where type == \"run\" | select run, app; where type "
           "== \"ev\" | lookup runs on run | select i, app";
}

// Usage records hold last use in microseconds; keep successive uses apart.
void tick() { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }

// Key, value, key and usage record, as persist_lookup counts an entry.
std::uint64_t entry_bytes(std::size_t value_size) {
    return 9 + value_size + 9 + 16;
}

}  // namespace

TEST_SUITE("LookupStore") {
    TEST_CASE("a lookup signature of an unresolved plan ends the process" *
              doctest::skip()) {
        fresh_process::announce_abort();
        views::detail::ViewPlan plan;
        (void)trace::index::cache::lookup_signature(plan, "side");
    }

    TEST_CASE("a lookup signature of an unresolved plan is fatal") {
        auto result = fresh_process::run(
            "a lookup signature of an unresolved plan ends the process");
        CHECK(result.aborted_with("the plan's record schema is not resolved"));
    }

    TEST_CASE("a stored side reads back inside the index root") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env, 30);
        const auto plan = plan_of(gz);
        const auto path = trace::index::cache::lookup_cache_path(*plan);
        CHECK(path == dftracer::utils::trace::index::lookups_dir(
                          dstd::String(determine_index_path(gz, ""))));
        const auto sig = trace::index::cache::lookup_signature(
            *plan, "where type == \"run\"");
        REQUIRE(sig);
        CHECK(trace::index::cache::lookup_signature(
                  *plan, "where type == \"job\"") != sig);

        auto db = trace::index::cache::open_lookup_db(
            path, trace::index::cache::CacheStore::Mode::ReadWrite);
        REQUIRE(db);
        CHECK_FALSE(
            dftu_utils_test::ok(trace::index::cache::lookup_exists(*db, *sig)));
        CHECK_FALSE(
            dftu_utils_test::ok(trace::index::cache::read_lookup(*db, *sig)));
        dftu_utils_test::ok(trace::index::cache::persist_lookup(
            *db, *sig, dstd::String("ipc\0bytes", 9)));
        CHECK(
            dftu_utils_test::ok(trace::index::cache::lookup_exists(*db, *sig)));
        CHECK(dftu_utils_test::ok(trace::index::cache::read_lookup(
                  *db, *sig)) == dstd::String("ipc\0bytes", 9));
    }

    TEST_CASE("the store evicts the least recently used side") {
        TestEnvironment env(200);
        auto db = store_in(env);
        REQUIRE(db);
        const dstd::String value(1000, 'v');
        ::setenv("DFTRACER_UTILS_CACHE_MAX_BYTES",
                 dstd::to_string(2 * entry_bytes(value.size())).c_str(), 1);
        dftu_utils_test::ok(trace::index::cache::persist_lookup(*db, 1, value));
        tick();
        dftu_utils_test::ok(trace::index::cache::persist_lookup(*db, 2, value));
        tick();
        CHECK(dftu_utils_test::ok(trace::index::cache::read_lookup(*db, 1)) ==
              dstd::String(dstd::StringView(value)));
        tick();
        dftu_utils_test::ok(trace::index::cache::persist_lookup(*db, 3, value));
        ::unsetenv("DFTRACER_UTILS_CACHE_MAX_BYTES");
        CHECK(dftu_utils_test::ok(trace::index::cache::lookup_exists(*db, 1)));
        CHECK_FALSE(
            dftu_utils_test::ok(trace::index::cache::lookup_exists(*db, 2)));
        CHECK(dftu_utils_test::ok(trace::index::cache::lookup_exists(*db, 3)));
    }

    TEST_CASE("without a read the oldest side goes first") {
        TestEnvironment env(200);
        auto db = store_in(env);
        REQUIRE(db);
        const dstd::String value(1000, 'v');
        ::setenv("DFTRACER_UTILS_CACHE_MAX_BYTES",
                 dstd::to_string(2 * entry_bytes(value.size())).c_str(), 1);
        dftu_utils_test::ok(trace::index::cache::persist_lookup(*db, 1, value));
        tick();
        dftu_utils_test::ok(trace::index::cache::persist_lookup(*db, 2, value));
        tick();
        CHECK(dftu_utils_test::ok(trace::index::cache::lookup_exists(*db, 1)));
        dftu_utils_test::ok(trace::index::cache::persist_lookup(*db, 3, value));
        ::unsetenv("DFTRACER_UTILS_CACHE_MAX_BYTES");
        CHECK_FALSE(
            dftu_utils_test::ok(trace::index::cache::lookup_exists(*db, 1)));
        CHECK(dftu_utils_test::ok(trace::index::cache::lookup_exists(*db, 2)));
        CHECK(dftu_utils_test::ok(trace::index::cache::lookup_exists(*db, 3)));
    }

    TEST_CASE("a side larger than the whole budget is not kept") {
        TestEnvironment env(200);
        auto db = store_in(env);
        REQUIRE(db);
        ::setenv("DFTRACER_UTILS_CACHE_MAX_BYTES", "100", 1);
        dftu_utils_test::ok(trace::index::cache::persist_lookup(
            *db, 1, dstd::String(1000, 'v')));
        CHECK_FALSE(
            dftu_utils_test::ok(trace::index::cache::lookup_exists(*db, 1)));
        dftu_utils_test::ok(
            trace::index::cache::persist_lookup(*db, 2, "small"));
        ::unsetenv("DFTRACER_UTILS_CACHE_MAX_BYTES");
        CHECK(dftu_utils_test::ok(trace::index::cache::read_lookup(*db, 2)) ==
              "small");
    }

    TEST_CASE("re-indexing a file with new records changes the signature") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env, 30);
        const auto before =
            trace::index::cache::lookup_signature(*plan_of(gz), "side");
        REQUIRE(before);
        CHECK(trace::index::cache::lookup_signature(*plan_of(gz), "side") ==
              before);

        create_mixed_trace(env, 40, 20);
        const auto after =
            trace::index::cache::lookup_signature(*plan_of(gz), "side");
        REQUIRE(after);
        CHECK(*after != *before);
    }

    TEST_CASE("a deleted cache folder reopens empty") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env, 30);
        const auto plan = plan_of(gz);
        const auto path = trace::index::cache::lookup_cache_path(*plan);
        const auto sig = trace::index::cache::lookup_signature(*plan, "side");
        REQUIRE(sig);
        {
            auto db = trace::index::cache::open_lookup_db(
                path, trace::index::cache::CacheStore::Mode::ReadWrite);
            REQUIRE(db);
            dftu_utils_test::ok(
                trace::index::cache::persist_lookup(*db, *sig, "rows"));
        }
        dstd::fs::remove_all(path).discard();

        CHECK_FALSE(trace::index::cache::open_lookup_db(
            path, trace::index::cache::CacheStore::Mode::ReadOnly));
        auto db = trace::index::cache::open_lookup_db(
            path, trace::index::cache::CacheStore::Mode::ReadWrite);
        REQUIRE(db);
        CHECK_FALSE(
            dftu_utils_test::ok(trace::index::cache::lookup_exists(*db, *sig)));
        dftu_utils_test::ok(
            trace::index::cache::persist_lookup(*db, *sig, "rows"));
        CHECK(dftu_utils_test::ok(
                  trace::index::cache::read_lookup(*db, *sig)) == "rows");
    }

    TEST_CASE("repeated queries that hit a stored side add one data file") {
        TestEnvironment env(200);
        const dstd::String gz = lookup_trace(env);
        const dstd::String q = lookup_query();
        for (int query = 0; query < 20; ++query) {
            const View v = View::from_file(
                dstd::String(gz),
                dstd::String(dstd::String(determine_index_path(gz, ""))));
            const auto g = run(v.duql(q).lazy().collect());
            CHECK(g.num_rows() == 4);
        }
        const auto store = dftracer::utils::trace::index::store_dir(
            dftracer::utils::trace::index::lookups_dir(
                dstd::String(determine_index_path(gz, ""))));
        REQUIRE(dstd::fs::exists(store).value());
        std::size_t files = 0;
        for (const auto& entry : dftu_utils_test::list_dir(store))
            if (entry.path.view().extension() == ".dfs") ++files;
        CHECK(files == 1);
    }

    TEST_CASE("a store written after reads is seen by the next read") {
        TestEnvironment env(200);
        const dstd::String path = env.dir() + "/lookups";
        {
            auto db = trace::index::cache::open_lookup_db(
                path, trace::index::cache::CacheStore::Mode::ReadWrite);
            REQUIRE(db);
            dftu_utils_test::ok(
                trace::index::cache::persist_lookup(*db, 1, "one"));
        }
        {
            auto ro = trace::index::cache::open_lookup_db(
                path, trace::index::cache::CacheStore::Mode::ReadOnly);
            REQUIRE(ro);
            CHECK(dftu_utils_test::ok(
                trace::index::cache::lookup_exists(*ro, 1)));
            CHECK_FALSE(dftu_utils_test::ok(
                trace::index::cache::lookup_exists(*ro, 2)));
        }
        {
            auto rw = trace::index::cache::open_lookup_db(
                path, trace::index::cache::CacheStore::Mode::ReadWrite);
            REQUIRE(rw);
            dftu_utils_test::ok(
                trace::index::cache::persist_lookup(*rw, 2, "two"));
        }
        auto ro = trace::index::cache::open_lookup_db(
            path, trace::index::cache::CacheStore::Mode::ReadOnly);
        REQUIRE(ro);
        CHECK(dftu_utils_test::ok(trace::index::cache::lookup_exists(*ro, 2)));
    }

    TEST_CASE("a store that cannot be opened is null") {
        TestEnvironment env(200);
        const dstd::String blocker = env.dir() + "/blocker";
        dftu_utils_test::FileOut(blocker) << "x";
        CHECK_FALSE(trace::index::cache::open_lookup_db(
            blocker + "/lookups",
            trace::index::cache::CacheStore::Mode::ReadWrite));
    }
}
