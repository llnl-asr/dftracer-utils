#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/async_once.h>
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>

#include <atomic>
#include <memory>
#include <vector>

using namespace dftracer::utils;
using dftu_utils_test::run_coro;

TEST_SUITE("AsyncOnce") {
    TEST_CASE("runs the producer once for concurrent callers") {
        coro::AsyncOnce<std::shared_ptr<int>> once;
        std::atomic<int> calls{0};
        const int n = 32;
        std::vector<std::shared_ptr<int>> results(n);

        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            std::vector<coro::CoroTask<std::shared_ptr<int>>> tasks;
            for (int i = 0; i < n; ++i) {
                tasks.push_back(
                    once.get([&]() -> coro::CoroTask<std::shared_ptr<int>> {
                        calls.fetch_add(1, std::memory_order_relaxed);
                        for (int k = 0; k < 4; ++k) co_await coro::yield();
                        co_return std::make_shared<int>(99);
                    }));
            }
            auto out = co_await coro::when_all(std::move(tasks));
            for (int i = 0; i < n; ++i) results[i] = out[i];
            co_return;
        });

        CHECK(calls.load() == 1);
        REQUIRE(results[0] != nullptr);
        CHECK(*results[0] == 99);
        for (int i = 1; i < n; ++i) CHECK(results[i] == results[0]);
    }

    TEST_CASE("returns the value to a late caller") {
        coro::AsyncOnce<int> once;
        std::atomic<int> calls{0};

        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            int a = co_await once.get([&]() -> coro::CoroTask<int> {
                calls.fetch_add(1, std::memory_order_relaxed);
                co_return 7;
            });
            // Second call after completion: no new producer run.
            int b = co_await once.get([&]() -> coro::CoroTask<int> {
                calls.fetch_add(1, std::memory_order_relaxed);
                co_return 7;
            });
            CHECK(a == 7);
            CHECK(b == 7);
            co_return;
        });

        CHECK(calls.load() == 1);
    }
}
