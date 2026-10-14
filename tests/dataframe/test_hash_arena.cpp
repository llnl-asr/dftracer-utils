// HashArena and the lazy unique() cursor built on it: keys that share a hash
// still dedup by their bytes, and unique() keeps the first row per key in
// input order when keys are the same length.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazy/hash_arena.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

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
}
