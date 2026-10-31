#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/cache/lookup_store.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <trace/views/test_view_common.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace {

namespace rdb = dftracer::utils::index::store;
namespace cache = dftracer::utils::index::cache;
namespace scan = dftracer::utils::trace::views::detail::scan;

std::string indexed_trace(TestEnvironment& env, int posix_n) {
    const std::string gz = create_mixed_trace(env, posix_n, 20);
    dftracer::utils::index::Indexer::open({gz}).build();
    return gz;
}

auto plan_of(const std::string& gz) {
    auto p = scan::from_file(gz, determine_index_path(gz, ""));
    dftracer::utils::trace::views::detail::plan_record_schema(*p);
    return p;
}

std::shared_ptr<rdb::RocksDatabase> store_in(TestEnvironment& env) {
    return cache::open_lookup_db(env.get_dir() + "/lookups",
                                 rdb::RocksDatabase::OpenMode::ReadWrite);
}

std::string lookup_trace(TestEnvironment& env) {
    const std::string plain = env.get_dir() + "/side.ndjson";
    {
        std::ofstream o(plain);
        o << R"({"type":"run","run":1,"app":"laghos"})" << "\n";
        o << R"({"type":"run","run":2,"app":"amg"})" << "\n";
        for (int i = 0; i < 4; ++i)
            o << R"({"type":"ev","i":)" << i << R"(,"run":)" << i % 2 + 1
              << "}\n";
    }
    const std::string gz = plain + ".gz";
    dftu_utils_test::compress_file_to_gzip(plain, gz);
    fs::remove(plain);
    dftracer::utils::index::Indexer::open({gz}).build();
    return gz;
}

std::string lookup_query() {
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
    TEST_CASE("a stored side reads back beside the index") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env, 30);
        const auto plan = plan_of(gz);
        const std::string path = cache::lookup_cache_path(*plan);
        CHECK(path ==
              (fs::path(gz).parent_path() / ".dftindex-cache" / "lookups")
                  .string());
        const auto sig =
            cache::lookup_signature(*plan, "where type == \"run\"");
        REQUIRE(sig);
        CHECK(cache::lookup_signature(*plan, "where type == \"job\"") != sig);

        auto db = cache::open_lookup_db(
            path, rdb::RocksDatabase::OpenMode::ReadWrite);
        REQUIRE(db);
        CHECK_FALSE(cache::lookup_exists(*db, *sig));
        CHECK_FALSE(cache::read_lookup(*db, *sig));
        cache::persist_lookup(*db, *sig, std::string("ipc\0bytes", 9));
        CHECK(cache::lookup_exists(*db, *sig));
        CHECK(cache::read_lookup(*db, *sig) == std::string("ipc\0bytes", 9));
    }

    TEST_CASE("the store evicts the least recently used side") {
        TestEnvironment env(200);
        auto db = store_in(env);
        REQUIRE(db);
        const std::string value(1000, 'v');
        ::setenv("DFTRACER_UTILS_CACHE_MAX_BYTES",
                 std::to_string(2 * entry_bytes(value.size())).c_str(), 1);
        cache::persist_lookup(*db, 1, value);
        tick();
        cache::persist_lookup(*db, 2, value);
        tick();
        CHECK(cache::read_lookup(*db, 1) == value);
        tick();
        cache::persist_lookup(*db, 3, value);
        ::unsetenv("DFTRACER_UTILS_CACHE_MAX_BYTES");
        CHECK(cache::lookup_exists(*db, 1));
        CHECK_FALSE(cache::lookup_exists(*db, 2));
        CHECK(cache::lookup_exists(*db, 3));
    }

    TEST_CASE("without a read the oldest side goes first") {
        TestEnvironment env(200);
        auto db = store_in(env);
        REQUIRE(db);
        const std::string value(1000, 'v');
        ::setenv("DFTRACER_UTILS_CACHE_MAX_BYTES",
                 std::to_string(2 * entry_bytes(value.size())).c_str(), 1);
        cache::persist_lookup(*db, 1, value);
        tick();
        cache::persist_lookup(*db, 2, value);
        tick();
        CHECK(cache::lookup_exists(*db, 1));
        cache::persist_lookup(*db, 3, value);
        ::unsetenv("DFTRACER_UTILS_CACHE_MAX_BYTES");
        CHECK_FALSE(cache::lookup_exists(*db, 1));
        CHECK(cache::lookup_exists(*db, 2));
        CHECK(cache::lookup_exists(*db, 3));
    }

    TEST_CASE("a side larger than the whole budget is not kept") {
        TestEnvironment env(200);
        auto db = store_in(env);
        REQUIRE(db);
        ::setenv("DFTRACER_UTILS_CACHE_MAX_BYTES", "100", 1);
        cache::persist_lookup(*db, 1, std::string(1000, 'v'));
        CHECK_FALSE(cache::lookup_exists(*db, 1));
        cache::persist_lookup(*db, 2, "small");
        ::unsetenv("DFTRACER_UTILS_CACHE_MAX_BYTES");
        CHECK(cache::read_lookup(*db, 2) == "small");
    }

    TEST_CASE("re-indexing a file with new records changes the signature") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env, 30);
        const auto before = cache::lookup_signature(*plan_of(gz), "side");
        REQUIRE(before);
        CHECK(cache::lookup_signature(*plan_of(gz), "side") == before);

        create_mixed_trace(env, 40, 20);
        const auto after = cache::lookup_signature(*plan_of(gz), "side");
        REQUIRE(after);
        CHECK(*after != *before);
    }

    TEST_CASE("a deleted cache folder reopens empty") {
        TestEnvironment env(200);
        const auto gz = indexed_trace(env, 30);
        const auto plan = plan_of(gz);
        const std::string path = cache::lookup_cache_path(*plan);
        const auto sig = cache::lookup_signature(*plan, "side");
        REQUIRE(sig);
        {
            auto db = cache::open_lookup_db(
                path, rdb::RocksDatabase::OpenMode::ReadWrite);
            REQUIRE(db);
            cache::persist_lookup(*db, *sig, "rows");
        }
        rdb::RocksDBManager::instance().reset(path);
        fs::remove_all(fs::path(path).parent_path());

        CHECK_FALSE(cache::open_lookup_db(
            path, rdb::RocksDatabase::OpenMode::ReadOnly));
        auto db = cache::open_lookup_db(
            path, rdb::RocksDatabase::OpenMode::ReadWrite);
        REQUIRE(db);
        CHECK_FALSE(cache::lookup_exists(*db, *sig));
        cache::persist_lookup(*db, *sig, "rows");
        CHECK(cache::read_lookup(*db, *sig) == "rows");
    }

    TEST_CASE("repeated queries that hit a stored side open the store twice") {
        TestEnvironment env(200);
        const std::string gz = lookup_trace(env);
        const std::string q = lookup_query();
        auto& manager = rdb::RocksDBManager::instance();
        const auto before = manager.opens();
        for (int query = 0; query < 20; ++query) {
            const View v = View::from_file(gz, determine_index_path(gz, ""));
            const auto g = run(v.duql(q).lazy().collect());
            CHECK(g.num_rows() == 4);
        }
        // One read-write open to store the side, then one read-only open
        // that every later query reuses.
        CHECK(manager.opens() - before <= 2);
    }

    TEST_CASE("queries leave the store's write lock free") {
        TestEnvironment env(200);
        const std::string gz = lookup_trace(env);
        const View v = View::from_file(gz, determine_index_path(gz, ""));
        for (int query = 0; query < 3; ++query)
            CHECK(run(v.duql(lookup_query()).lazy().collect()).num_rows() == 4);
        const std::string path =
            (fs::path(determine_index_path(gz, "")).parent_path() /
             ".dftindex-cache" / "lookups")
                .string();
        REQUIRE(fs::exists(path));

        rdb::RocksDatabase other;
        CHECK_NOTHROW(
            other.open(path, rdb::RocksDatabase::OpenMode::ReadWrite));
    }

    TEST_CASE("a store written after reads is seen by the next read") {
        TestEnvironment env(200);
        const std::string path = env.get_dir() + "/lookups";
        {
            auto db = cache::open_lookup_db(
                path, rdb::RocksDatabase::OpenMode::ReadWrite);
            REQUIRE(db);
            cache::persist_lookup(*db, 1, "one");
        }
        {
            auto ro = cache::open_lookup_db(
                path, rdb::RocksDatabase::OpenMode::ReadOnly);
            REQUIRE(ro);
            CHECK(cache::lookup_exists(*ro, 1));
            CHECK_FALSE(cache::lookup_exists(*ro, 2));
        }
        {
            auto rw = cache::open_lookup_db(
                path, rdb::RocksDatabase::OpenMode::ReadWrite);
            REQUIRE(rw);
            cache::persist_lookup(*rw, 2, "two");
        }
        auto ro =
            cache::open_lookup_db(path, rdb::RocksDatabase::OpenMode::ReadOnly);
        REQUIRE(ro);
        CHECK(cache::lookup_exists(*ro, 2));
    }

    TEST_CASE("a store that cannot be opened is null") {
        TestEnvironment env(200);
        const std::string blocker = env.get_dir() + "/blocker";
        std::ofstream(blocker) << "x";
        CHECK_FALSE(cache::open_lookup_db(
            blocker + "/lookups", rdb::RocksDatabase::OpenMode::ReadWrite));
    }
}
