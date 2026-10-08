#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/memory_pool.h>
#include <doctest/doctest.h>

#include <thread>
#include <vector>

using dftracer::utils::MemoryPool;
using dftracer::utils::Reservation;

TEST_CASE("a reserve above the capacity fails and is not clamped") {
    MemoryPool pool(100);
    CHECK_FALSE(pool.try_reserve(101));
    CHECK(pool.used() == 0);
    CHECK(pool.try_reserve(100));
    CHECK_FALSE(pool.try_reserve(1));
}

TEST_CASE("a reservation returns its bytes on destruction and reset") {
    MemoryPool pool(100);
    {
        Reservation r(pool);
        CHECK(r.try_grow(60));
        CHECK_FALSE(r.try_grow(41));
        CHECK(pool.used() == 60);
        Reservation moved = std::move(r);
        CHECK(r.bytes() == 0);
        CHECK(moved.bytes() == 60);
        CHECK(pool.used() == 60);
        moved.reset();
        CHECK(pool.used() == 0);
    }
    CHECK(pool.used() == 0);
}

TEST_CASE("concurrent reserves never exceed the capacity") {
    MemoryPool pool(1000);
    std::vector<std::thread> ts;
    std::atomic<std::uint64_t> granted{0};
    for (int t = 0; t < 8; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 1000; ++i)
                if (pool.try_reserve(7)) granted += 7;
        });
    for (auto& t : ts) t.join();
    CHECK(granted.load() == pool.used());
    CHECK(pool.used() <= 1000);
}
