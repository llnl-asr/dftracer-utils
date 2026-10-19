// HashArena and the lazy unique() cursor built on it: keys that share a hash
// still dedup by their bytes, and unique() keeps the first row per key in
// input order when keys are the same length.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazy/hash_arena.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::lazy_internal::HashArena;

namespace {

bool put(HashArena& a, std::uint64_t hash, const std::string& key) {
    return a.insert(hash, key.data(), static_cast<std::uint32_t>(key.size()));
}

bool has(const HashArena& a, std::uint64_t hash, const std::string& key) {
    return a.contains(hash, key.data(), static_cast<std::uint32_t>(key.size()));
}

}  // namespace

TEST_SUITE("hash arena") {
    TEST_CASE("keys with one hash are told apart by their bytes") {
        HashArena a;
        constexpr std::uint64_t H = 42;
        CHECK(put(a, H, "abcd"));
        CHECK(put(a, H, "abce"));  // same hash, same length, other bytes
        CHECK(put(a, H, "abc"));   // same hash, other length
        CHECK(put(a, H, ""));      // an empty key is a key
        CHECK_FALSE(put(a, H, "abcd"));
        CHECK_FALSE(put(a, H, "abce"));
        CHECK_FALSE(put(a, H, "abc"));
        CHECK_FALSE(put(a, H, ""));
        CHECK(has(a, H, "abce"));
        CHECK_FALSE(has(a, H, "abcf"));
        CHECK_FALSE(has(a, H + 1, "abcd"));
    }

    TEST_CASE("many colliding keys all stay distinct and findable") {
        HashArena a;
        for (int i = 0; i < 200; ++i) CHECK(put(a, 7, "k" + std::to_string(i)));
        for (int i = 0; i < 200; ++i) {
            CHECK_FALSE(put(a, 7, "k" + std::to_string(i)));
            CHECK(has(a, 7, "k" + std::to_string(i)));
        }
    }

    TEST_CASE("lazy unique keeps the first of equal-length keys in order") {
        // Row keys ("ab","c") and ("a","bc") have the same total bytes and
        // would be the same row if the cell boundary were not part of the key.
        const std::vector<std::string> a{"ab", "a",  "ab", "xy",
                                         "a",  "xy", "ab"};
        const std::vector<std::string> b{"c", "bc", "c", "zz", "bc", "zw", "c"};
        DataFrame df;
        df.names = {"a", "b"};
        df.columns.push_back(Series::strings(a));
        df.columns.push_back(Series::strings(b));
        DataFrame out = dftracer::utils::default_runtime()
                            .submit(df.lazy().unique().collect(2))
                            .get();
        REQUIRE(out.num_rows() == 4);
        const std::vector<std::pair<std::string, std::string>> want{
            {"ab", "c"}, {"a", "bc"}, {"xy", "zz"}, {"xy", "zw"}};
        for (std::int64_t i = 0; i < 4; ++i) {
            CHECK(out.column("a").string_at(i) ==
                  want[static_cast<std::size_t>(i)].first);
            CHECK(out.column("b").string_at(i) ==
                  want[static_cast<std::size_t>(i)].second);
        }
    }

    TEST_CASE("an arena charges its pool and frees all of it") {
        dftracer::utils::memory::PoolHandle pool =
            dftracer::utils::memory::Manager::get().root().child("arena");
        REQUIRE(pool);
        {
            HashArena a(*pool);
            for (int i = 0; i < 5000; ++i)
                CHECK(put(a, static_cast<std::uint64_t>(i) * 2654435761u,
                          "key" + std::to_string(i)));
            CHECK(pool->live() >= a.bytes());
            CHECK(a.bytes() > 0);
        }
        CHECK(pool->live() == 0);
        CHECK(pool->blocks() == 0);
    }

    TEST_CASE("keys stay in place while the arena grows") {
        dftracer::utils::memory::PoolHandle pool =
            dftracer::utils::memory::Manager::get().root().child("grow");
        REQUIRE(pool);
        HashArena a(*pool);
        // About 80 bytes a record: enough keys to fill the 64 KiB to 1 MiB
        // chunks and start the next ones.
        constexpr int KEYS = 40000;
        auto key_of = [](int i) {
            return "key-" + std::to_string(i) + std::string(56, 'x');
        };
        // One hash for two keys: the chain must keep them apart.
        constexpr std::uint64_t SHARED = 99;
        const std::string first = "shared-" + key_of(0);
        const std::string second = "shared-" + key_of(1);
        std::size_t key_bytes = first.size() + second.size();
        std::size_t key_max = std::max(first.size(), second.size());
        for (int i = 0; i < KEYS; ++i) {
            key_bytes += key_of(i).size();
            key_max = std::max(key_max, key_of(i).size());
        }
        const std::uint64_t bound = a.bytes_after(KEYS + 2, key_bytes, key_max);
        REQUIRE(put(a, SHARED, first));
        for (int i = 0; i < KEYS; ++i)
            REQUIRE(put(a, static_cast<std::uint64_t>(i) * 2654435761u + 1,
                        key_of(i)));
        REQUIRE(put(a, SHARED, second));
        CHECK(a.bytes() > (std::uint64_t{2} << 20));
        CHECK(a.bytes() <= bound);
        bool all = true;
        for (int i = 0; i < KEYS; ++i)
            all = all && has(a, static_cast<std::uint64_t>(i) * 2654435761u + 1,
                             key_of(i));
        CHECK(all);
        CHECK(has(a, SHARED, first));
        CHECK(has(a, SHARED, second));
        CHECK_FALSE(has(a, SHARED, "shared-" + key_of(2)));
        CHECK_FALSE(put(a, SHARED, first));
    }

    TEST_CASE("the table doubles while small and grows four times over after") {
        HashArena a;
        std::uint64_t at = 0;
        int large = 0;
        bool bounded = true;
        while (large < 2) {
            const std::size_t before = a.table_buckets();
            const std::string key = "k" + std::to_string(at);
            const std::uint64_t bound =
                a.bytes_after(1, key.size(), key.size());
            REQUIRE(put(a, ++at, key));
            bounded = bounded && a.bytes() <= bound;
            if (a.table_buckets() == before || before == 0) continue;
            if (before < HashArena::GROWTH_FROM) {
                CHECK(a.table_buckets() == 2 * before);
            } else {
                CHECK(a.table_buckets() == 4 * before);
                ++large;
            }
        }
        CHECK(bounded);
        bool all = true;
        for (std::uint64_t k = 1; k <= at; ++k)
            all = all && has(a, k, "k" + std::to_string(k - 1));
        CHECK(all);
    }

    TEST_CASE("a small key set reserves no table four times its size") {
        // The keys unique() takes from one morsel before a 4 MiB budget
        // spills: a table grown four times at once held 7.1 MiB here.
        dftracer::utils::memory::PoolHandle pool =
            dftracer::utils::memory::Manager::get().root().child("small");
        REQUIRE(pool);
        HashArena a(*pool);
        for (std::uint64_t i = 0; i < 57000; ++i) {
            const std::string key = std::to_string(i * 2654435761u);
            REQUIRE(put(a, i * 0x9e3779b97f4a7c15ULL, key));
        }
        CHECK(a.bytes() < (std::uint64_t{5} << 20));
        CHECK(pool->live() >= a.bytes());
    }

    TEST_CASE("bytes_after bounds what the inserts leave") {
        HashArena a;
        std::uint64_t at = 0;
        for (int round = 0; round < 20; ++round) {
            const std::size_t keys = 1u << (round % 12);
            std::vector<std::string> batch;
            std::size_t key_bytes = 0;
            for (std::size_t k = 0; k < keys; ++k) {
                batch.push_back("r" + std::to_string(round) + "k" +
                                std::to_string(k));
                key_bytes += batch.back().size();
            }
            const std::uint64_t bound = a.bytes_after(keys, key_bytes);
            for (const std::string& key : batch) put(a, ++at, key);
            CHECK(a.bytes() <= bound);
        }
    }
}
