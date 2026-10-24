#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/cache/eviction_policies/clock_eviction_policy.h>
#include <dftracer/utils/dstd/cache/eviction_policies/lru_eviction_policy.h>
#include <doctest/doctest.h>

using dftracer::utils::cache::ClockEvictionPolicy;
using dftracer::utils::cache::LruEvictionPolicy;

TEST_SUITE("eviction_policy") {
    TEST_CASE("LRU evicts the least recently used") {
        LruEvictionPolicy<int> lru;
        lru.touch(1);
        lru.touch(2);
        lru.touch(3);    // order MRU..LRU: 3, 2, 1
        CHECK(lru.size() == 3);

        lru.touch(1);    // 1 becomes MRU: 1, 3, 2
        auto v = lru.pop_victim();
        REQUIRE(v.has_value());
        CHECK(*v == 2);  // 2 is now least recently used

        v = lru.pop_victim();
        REQUIRE(v.has_value());
        CHECK(*v == 3);
        CHECK(lru.size() == 1);
    }

    TEST_CASE("LRU remove drops a key") {
        LruEvictionPolicy<int> lru;
        lru.touch(1);
        lru.touch(2);
        lru.remove(1);
        CHECK(lru.size() == 1);
        auto v = lru.pop_victim();
        REQUIRE(v.has_value());
        CHECK(*v == 2);
        CHECK_FALSE(lru.pop_victim().has_value());
    }

    TEST_CASE("CLOCK gives a touched key a second chance") {
        ClockEvictionPolicy<int> clock;
        clock.touch(1);  // ref=1
        clock.touch(2);  // ref=1
        clock.touch(3);  // ref=1

        // First sweep clears 1,2,3 then wraps and evicts 1 (ref now clear).
        auto v = clock.pop_victim();
        REQUIRE(v.has_value());
        CHECK(*v == 1);

        // 2's bit was cleared during the sweep; re-touch it for a second
        // chance so 3 is evicted next instead.
        clock.touch(2);  // ref=1 again
        v = clock.pop_victim();
        REQUIRE(v.has_value());
        CHECK(*v == 3);

        v = clock.pop_victim();
        REQUIRE(v.has_value());
        CHECK(*v == 2);
        CHECK(clock.size() == 0);
        CHECK_FALSE(clock.pop_victim().has_value());
    }

    TEST_CASE("CLOCK remove keeps the hand valid") {
        ClockEvictionPolicy<int> clock;
        for (int i = 0; i < 5; ++i) clock.touch(i);
        clock.remove(2);
        clock.remove(4);
        CHECK(clock.size() == 3);
        auto a = clock.pop_victim();
        auto b = clock.pop_victim();
        auto c = clock.pop_victim();
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE(c.has_value());
        CHECK_FALSE(clock.pop_victim().has_value());
    }
}
