// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/index/cache/lookup_store.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>
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
    dftracer::utils::index::Indexer::open({dstd::String(dstd::StringView(gz))})
        .build();
    return gz;
}

auto plan_of(const dstd::String& gz) {
    auto p = views::detail::scan::from_file(
        dstd::String(gz),
        dstd::String(dstd::String(determine_index_path(gz, ""))));
    dftracer::utils::trace::views::detail::plan_record_schema(*p);
    return p;
}

dstd::SharedPtr<index::cache::CacheStore> store_in(TestEnvironment& env) {
    return index::cache::open_lookup_db(
        env.get_dir() + "/lookups", index::cache::CacheStore::Mode::ReadWrite);
}

dstd::String lookup_trace(TestEnvironment& env) {
    const dstd::String plain = env.get_dir() + "/side.ndjson";
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
    (void)dstd::fs::remove(plain);
    dftracer::utils::index::Indexer::open({dstd::String(dstd::StringView(gz))})
        .build();
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
    TEST_CASE("a stored side reads back inside the index root") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env, 30);
        const auto plan = plan_of(gz);
        const auto path = index::cache::lookup_cache_path(*plan);
        CHECK(path == dftracer::utils::index::lookups_dir(
                          dstd::String(determine_index_path(gz, ""))));
        const auto sig =
            index::cache::lookup_signature(*plan, "where type == \"run\"");
        REQUIRE(sig);
        CHECK(index::cache::lookup_signature(*plan, "where type == \"job\"") !=
              sig);

        auto db = index::cache::open_lookup_db(
            path, index::cache::CacheStore::Mode::ReadWrite);
        REQUIRE(db);
        CHECK_FALSE(index::cache::lookup_exists(*db, *sig));
        CHECK_FALSE(index::cache::read_lookup(*db, *sig));
        index::cache::persist_lookup(*db, *sig, dstd::String("ipc\0bytes", 9));
        CHECK(index::cache::lookup_exists(*db, *sig));
        CHECK(index::cache::read_lookup(*db, *sig) ==
              dstd::String("ipc\0bytes", 9));
    }

    TEST_CASE("the store evicts the least recently used side") {
        TestEnvironment env(200);
        auto db = store_in(env);
        REQUIRE(db);
        const dstd::String value(1000, 'v');
        ::setenv("DFTRACER_UTILS_CACHE_MAX_BYTES",
                 dstd::to_string(2 * entry_bytes(value.size())).c_str(), 1);
        index::cache::persist_lookup(*db, 1, value);
        tick();
        index::cache::persist_lookup(*db, 2, value);
        tick();
        CHECK(index::cache::read_lookup(*db, 1) ==
              dstd::String(dstd::StringView(value)));
        tick();
        index::cache::persist_lookup(*db, 3, value);
        ::unsetenv("DFTRACER_UTILS_CACHE_MAX_BYTES");
        CHECK(index::cache::lookup_exists(*db, 1));
        CHECK_FALSE(index::cache::lookup_exists(*db, 2));
        CHECK(index::cache::lookup_exists(*db, 3));
    }

    TEST_CASE("without a read the oldest side goes first") {
        TestEnvironment env(200);
        auto db = store_in(env);
        REQUIRE(db);
        const dstd::String value(1000, 'v');
        ::setenv("DFTRACER_UTILS_CACHE_MAX_BYTES",
                 dstd::to_string(2 * entry_bytes(value.size())).c_str(), 1);
        index::cache::persist_lookup(*db, 1, value);
        tick();
        index::cache::persist_lookup(*db, 2, value);
        tick();
        CHECK(index::cache::lookup_exists(*db, 1));
        index::cache::persist_lookup(*db, 3, value);
        ::unsetenv("DFTRACER_UTILS_CACHE_MAX_BYTES");
        CHECK_FALSE(index::cache::lookup_exists(*db, 1));
        CHECK(index::cache::lookup_exists(*db, 2));
        CHECK(index::cache::lookup_exists(*db, 3));
    }

    TEST_CASE("a side larger than the whole budget is not kept") {
        TestEnvironment env(200);
        auto db = store_in(env);
        REQUIRE(db);
        ::setenv("DFTRACER_UTILS_CACHE_MAX_BYTES", "100", 1);
        index::cache::persist_lookup(*db, 1, dstd::String(1000, 'v'));
        CHECK_FALSE(index::cache::lookup_exists(*db, 1));
        index::cache::persist_lookup(*db, 2, "small");
        ::unsetenv("DFTRACER_UTILS_CACHE_MAX_BYTES");
        CHECK(index::cache::read_lookup(*db, 2) == "small");
    }

    TEST_CASE("re-indexing a file with new records changes the signature") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env, 30);
        const auto before =
            index::cache::lookup_signature(*plan_of(gz), "side");
        REQUIRE(before);
        CHECK(index::cache::lookup_signature(*plan_of(gz), "side") == before);

        create_mixed_trace(env, 40, 20);
        const auto after = index::cache::lookup_signature(*plan_of(gz), "side");
        REQUIRE(after);
        CHECK(*after != *before);
    }

    TEST_CASE("a deleted cache folder reopens empty") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env, 30);
        const auto plan = plan_of(gz);
        const auto path = index::cache::lookup_cache_path(*plan);
        const auto sig = index::cache::lookup_signature(*plan, "side");
        REQUIRE(sig);
        {
            auto db = index::cache::open_lookup_db(
                path, index::cache::CacheStore::Mode::ReadWrite);
            REQUIRE(db);
            index::cache::persist_lookup(*db, *sig, "rows");
        }
        (void)dstd::fs::remove_all(path);

        CHECK_FALSE(index::cache::open_lookup_db(
            path, index::cache::CacheStore::Mode::ReadOnly));
        auto db = index::cache::open_lookup_db(
            path, index::cache::CacheStore::Mode::ReadWrite);
        REQUIRE(db);
        CHECK_FALSE(index::cache::lookup_exists(*db, *sig));
        index::cache::persist_lookup(*db, *sig, "rows");
        CHECK(index::cache::read_lookup(*db, *sig) == "rows");
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
        const auto store = dftracer::utils::index::store_dir(
            dftracer::utils::index::lookups_dir(
                dstd::String(determine_index_path(gz, ""))));
        REQUIRE(dstd::fs::exists(store).value());
        std::size_t files = 0;
        for (const auto& entry : dftu_utils_test::list_dir(store))
            if (entry.path.view().extension() == ".dfs") ++files;
        CHECK(files == 1);
    }

    TEST_CASE("a store written after reads is seen by the next read") {
        TestEnvironment env(200);
        const dstd::String path = env.get_dir() + "/lookups";
        {
            auto db = index::cache::open_lookup_db(
                path, index::cache::CacheStore::Mode::ReadWrite);
            REQUIRE(db);
            index::cache::persist_lookup(*db, 1, "one");
        }
        {
            auto ro = index::cache::open_lookup_db(
                path, index::cache::CacheStore::Mode::ReadOnly);
            REQUIRE(ro);
            CHECK(index::cache::lookup_exists(*ro, 1));
            CHECK_FALSE(index::cache::lookup_exists(*ro, 2));
        }
        {
            auto rw = index::cache::open_lookup_db(
                path, index::cache::CacheStore::Mode::ReadWrite);
            REQUIRE(rw);
            index::cache::persist_lookup(*rw, 2, "two");
        }
        auto ro = index::cache::open_lookup_db(
            path, index::cache::CacheStore::Mode::ReadOnly);
        REQUIRE(ro);
        CHECK(index::cache::lookup_exists(*ro, 2));
    }

    TEST_CASE("a store that cannot be opened is null") {
        TestEnvironment env(200);
        const dstd::String blocker = env.get_dir() + "/blocker";
        dftu_utils_test::FileOut(blocker) << "x";
        CHECK_FALSE(index::cache::open_lookup_db(
            blocker + "/lookups", index::cache::CacheStore::Mode::ReadWrite));
    }
}
