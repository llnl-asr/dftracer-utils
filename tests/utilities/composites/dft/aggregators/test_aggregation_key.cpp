#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_key.h>

#include <unordered_map>

using namespace dftracer::utils::utilities::composites::dft::aggregators;

static AggregationKey make_key(
    const std::string& cat = "cat1", const std::string& name = "name1",
    std::uint64_t pid = 1, std::uint64_t tid = 1,
    const std::string& hhash = "hh1", const std::string& fhash = "fh1",
    std::uint64_t time_bucket = 0,
    const std::unordered_map<std::string, std::string>& extra = {}) {
    return AggregationKey{cat, name, pid, tid, hhash, fhash, time_bucket,
                          extra};
}

TEST_SUITE("AggregationKey") {
    TEST_CASE("AggregationKey - Equality") {
        auto k1 = make_key();
        auto k2 = make_key();
        CHECK(k1 == k2);
    }

    TEST_CASE("AggregationKey - Inequality") {
        auto base = make_key();

        SUBCASE("Different cat") {
            auto other = make_key("cat2");
            CHECK_FALSE(base == other);
        }

        SUBCASE("Different name") {
            auto other = make_key("cat1", "name2");
            CHECK_FALSE(base == other);
        }

        SUBCASE("Different pid") {
            auto other = make_key("cat1", "name1", 99);
            CHECK_FALSE(base == other);
        }

        SUBCASE("Different tid") {
            auto other = make_key("cat1", "name1", 1, 99);
            CHECK_FALSE(base == other);
        }

        SUBCASE("Different hhash") {
            auto other = make_key("cat1", "name1", 1, 1, "hh_other");
            CHECK_FALSE(base == other);
        }

        SUBCASE("Different fhash") {
            auto other = make_key("cat1", "name1", 1, 1, "hh1", "fh_other");
            CHECK_FALSE(base == other);
        }

        SUBCASE("Different time_bucket") {
            auto other = make_key("cat1", "name1", 1, 1, "hh1", "fh1", 999);
            CHECK_FALSE(base == other);
        }

        SUBCASE("Different extra_keys") {
            auto other = make_key("cat1", "name1", 1, 1, "hh1", "fh1", 0,
                                  {{"k", "v"}});
            CHECK_FALSE(base == other);
        }
    }

    TEST_CASE("AggregationKey - Hash consistency") {
        AggregationKeyHash hasher;
        auto k1 = make_key();
        auto k2 = make_key();

        CHECK(hasher(k1) == hasher(k2));
        // Same key hashed multiple times
        CHECK(hasher(k1) == hasher(k1));
    }

    TEST_CASE("AggregationKey - Hash differs for different keys") {
        AggregationKeyHash hasher;
        auto k1 = make_key();

        SUBCASE("Different cat") {
            auto k2 = make_key("cat2");
            CHECK(hasher(k1) != hasher(k2));
        }

        SUBCASE("Different name") {
            auto k2 = make_key("cat1", "name2");
            CHECK(hasher(k1) != hasher(k2));
        }

        SUBCASE("Different pid") {
            auto k2 = make_key("cat1", "name1", 99);
            CHECK(hasher(k1) != hasher(k2));
        }
    }

    TEST_CASE("AggregationKey - Extra keys equality regardless of insertion order") {
        auto k1 = make_key("cat1", "name1", 1, 1, "hh1", "fh1", 0,
                           {{"a", "1"}, {"b", "2"}});
        auto k2 = make_key("cat1", "name1", 1, 1, "hh1", "fh1", 0,
                           {{"b", "2"}, {"a", "1"}});
        CHECK(k1 == k2);
    }

    TEST_CASE("AggregationKey - Use in unordered_map") {
        std::unordered_map<AggregationKey, int, AggregationKeyHash> map;

        auto k1 = make_key("cat1", "read");
        auto k2 = make_key("cat2", "write");

        map[k1] = 10;
        map[k2] = 20;

        CHECK(map.size() == 2);
        CHECK(map[k1] == 10);
        CHECK(map[k2] == 20);

        // Same key retrieves existing entry
        auto k1_copy = make_key("cat1", "read");
        CHECK(map[k1_copy] == 10);
    }
}
