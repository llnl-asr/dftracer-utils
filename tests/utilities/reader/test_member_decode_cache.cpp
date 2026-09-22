#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/reader/internal/member_decode_cache.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::utilities::reader::internal::MemberDecodeCache;
using dftu_utils_test::run_coro;
using Bytes = MemberDecodeCache::Bytes;

namespace {

// Producer that counts invocations and yields a few times so concurrent
// callers pile up on the in-flight entry before it completes.
MemberDecodeCache::Producer counting_producer(std::atomic<int>& calls,
                                              std::uint8_t fill,
                                              std::size_t size) {
    return [&calls, fill, size]() -> coro::CoroTask<Bytes> {
        calls.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < 4; ++i) co_await coro::yield();
        auto m = std::make_shared<
            dftracer::utils::utilities::reader::internal::DecodedMember>();
        m->data.assign(size, fill);
        m->compressed_size = size / 2;
        co_return m;
    };
}

}  // namespace

TEST_SUITE("MemberDecodeCache") {
    TEST_CASE("concurrent requests for one member decode once") {
        MemberDecodeCache cache(64 * 1024 * 1024);
        std::atomic<int> calls{0};
        const int n = 32;

        std::vector<Bytes> results(n);
        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            std::vector<coro::CoroTask<Bytes>> tasks;
            tasks.reserve(n);
            for (int i = 0; i < n; ++i) {
                tasks.push_back(cache.get_or_decode(
                    /*file_token=*/1, /*member_idx=*/7,
                    counting_producer(calls, 0xAB, 1024)));
            }
            auto out = co_await coro::when_all(std::move(tasks));
            for (int i = 0; i < n; ++i) results[i] = out[i];
            co_return;
        });

        // N concurrent requests, exactly one decode (before: N; after: 1).
        CHECK(calls.load() == 1);
        auto s = cache.stats();
        CHECK(s.requests == static_cast<std::uint64_t>(n));
        CHECK(s.decodes == 1);
        // Every caller got the same shared buffer with the right contents.
        REQUIRE(results[0] != nullptr);
        for (int i = 1; i < n; ++i) CHECK(results[i] == results[0]);
        CHECK(results[0]->data.size() == 1024);
        CHECK(results[0]->data[0] == 0xAB);
    }

    TEST_CASE("distinct members each decode once") {
        MemberDecodeCache cache(64 * 1024 * 1024);
        std::atomic<int> calls{0};
        const int n = 8;

        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            std::vector<coro::CoroTask<Bytes>> tasks;
            for (int i = 0; i < n; ++i) {
                tasks.push_back(cache.get_or_decode(
                    1, static_cast<std::uint64_t>(i),
                    counting_producer(calls, static_cast<std::uint8_t>(i),
                                      256)));
            }
            co_await coro::when_all(std::move(tasks));
            co_return;
        });

        CHECK(calls.load() == n);
        CHECK(cache.stats().decodes == static_cast<std::uint64_t>(n));
    }

    TEST_CASE("repeat request hits the cache (no re-decode)") {
        MemberDecodeCache cache(64 * 1024 * 1024);
        std::atomic<int> calls{0};

        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            auto a = co_await cache.get_or_decode(
                2, 3, counting_producer(calls, 0x11, 512));
            auto b = co_await cache.get_or_decode(
                2, 3, counting_producer(calls, 0x11, 512));
            CHECK(a == b);
            co_return;
        });

        CHECK(calls.load() == 1);
        CHECK(cache.stats().hits >= 1);
    }

    TEST_CASE("a small budget evicts under many members") {
        // Total budget far below the working set; spread across shards, some
        // shard fills and evicts. Each member is decoded once here (distinct
        // keys, sequential), so decodes == members and evictions > 0.
        MemberDecodeCache cache(/*capacity_bytes=*/4096);
        std::atomic<int> calls{0};
        const int members = 400;

        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            for (int m = 0; m < members; ++m) {
                co_await cache.get_or_decode(
                    9, static_cast<std::uint64_t>(m),
                    counting_producer(calls, static_cast<std::uint8_t>(m),
                                      256));
            }
            co_return;
        });

        auto s = cache.stats();
        CHECK(calls.load() == members);
        CHECK(s.decodes == static_cast<std::uint64_t>(members));
        CHECK(s.evictions > 0);
    }
}
