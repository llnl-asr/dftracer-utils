// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/hooks.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <stdexcept>

using namespace dftracer::utils;

namespace {

using IntResult = dstd::Expected<int, int>;

coro::CoroTask<IntResult> now(int v, bool fail) {
    if (fail) co_return dstd::Unexpected(v);
    co_return v;
}

coro::CoroTask<IntResult> late(int v, bool fail) {
    co_await coro::yield();
    if (fail) co_return dstd::Unexpected(v);
    co_return v;
}

struct Counted {
    std::atomic<int>* destroyed;
    ~Counted() { destroyed->fetch_add(1); }
};

template <typename T>
T run(Runtime& rt, coro::CoroTask<T> task) {
    return rt.submit(std::move(task)).get();
}

}  // namespace

TEST_SUITE("or_fail") {
    TEST_CASE("a value passes through") {
        Runtime rt(2);
        auto task = []() -> coro::CoroTask<IntResult> {
            const int v = co_await coro::or_fail(IntResult(3));
            co_return v + 1;
        };
        const IntResult r = run(rt, task());
        REQUIRE(r.has_value());
        CHECK(*r == 4);
    }

    TEST_CASE("an error ends the task and destroys its locals once") {
        Runtime rt(2);
        std::atomic<int> destroyed{0};
        std::atomic<bool> went_on{false};
        auto task = [&]() -> coro::CoroTask<IntResult> {
            Counted local{&destroyed};
            const int v =
                co_await coro::or_fail(IntResult(dstd::Unexpected(9)));
            went_on.store(true);
            co_return v;
        };
        const IntResult r = run(rt, task());
        REQUIRE(!r.has_value());
        CHECK(r.error() == 9);
        CHECK(!went_on.load());
        CHECK(destroyed.load() == 1);
    }

    TEST_CASE("a task that fails before it suspends ends its awaiter") {
        Runtime rt(2);
        std::atomic<bool> went_on{false};
        auto task = [&](bool fail) -> coro::CoroTask<IntResult> {
            const int v = co_await coro::or_fail(now(5, fail));
            went_on.store(true);
            co_return v * 2;
        };
        const IntResult ok = run(rt, task(false));
        REQUIRE(ok.has_value());
        CHECK(*ok == 10);
        went_on.store(false);
        const IntResult bad = run(rt, task(true));
        REQUIRE(!bad.has_value());
        CHECK(bad.error() == 5);
        CHECK(!went_on.load());
    }

    TEST_CASE("a task that fails after it suspends ends its awaiter") {
        Runtime rt(4);
        std::atomic<int> went_on{0};
        std::atomic<int> destroyed{0};
        auto task = [&](int i) -> coro::CoroTask<IntResult> {
            Counted local{&destroyed};
            const int v = co_await late(i, i % 2 == 1).or_fail();
            went_on.fetch_add(1);
            co_return v;
        };
        constexpr int ROUNDS = 400;
        for (int i = 0; i < ROUNDS; ++i) {
            const IntResult r = run(rt, task(i));
            if (i % 2 == 1) {
                REQUIRE(!r.has_value());
                CHECK(r.error() == i);
            } else {
                REQUIRE(r.has_value());
                CHECK(*r == i);
            }
        }
        CHECK(went_on.load() == ROUNDS / 2);
        CHECK(destroyed.load() == ROUNDS);
    }

    TEST_CASE("an error passes up a chain of tasks") {
        Runtime rt(2);
        auto inner = []() -> coro::CoroTask<IntResult> {
            co_return co_await coro::or_fail(late(7, true));
        };
        auto middle = [&]() -> coro::CoroTask<IntResult> {
            const int v = co_await coro::or_fail(inner());
            co_return v + 100;
        };
        auto outer = [&]() -> coro::CoroTask<IntResult> {
            const int v = co_await coro::or_fail(middle());
            co_return v + 1000;
        };
        const IntResult r = run(rt, outer());
        REQUIRE(!r.has_value());
        CHECK(r.error() == 7);
    }

    TEST_CASE("the error converts to the error type of the task") {
        Runtime rt(2);
        auto task = []() -> coro::CoroTask<dstd::Expected<int, long>> {
            co_return co_await coro::or_fail(now(11, true));
        };
        const auto r = run(rt, task());
        REQUIRE(!r.has_value());
        CHECK(r.error() == 11L);
    }

    TEST_CASE("a void value passes and fails") {
        Runtime rt(2);
        auto step = [](bool fail) -> coro::CoroTask<dstd::Expected<void, int>> {
            co_await coro::yield();
            if (fail) co_return dstd::Unexpected(3);
            co_return dstd::Expected<void, int>();
        };
        auto task = [&](bool fail) -> coro::CoroTask<IntResult> {
            co_await coro::or_fail(step(fail));
            co_return 1;
        };
        CHECK(*run(rt, task(false)) == 1);
        const IntResult bad = run(rt, task(true));
        REQUIRE(!bad.has_value());
        CHECK(bad.error() == 3);
    }

    TEST_CASE("a plain co_await of a failed task gives its error") {
        Runtime rt(2);
        auto task = []() -> coro::CoroTask<IntResult> {
            const IntResult first = co_await now(4, true);
            const IntResult second = co_await late(6, true);
            co_return first.error() + second.error();
        };
        const IntResult r = run(rt, task());
        REQUIRE(r.has_value());
        CHECK(*r == 10);
    }

    TEST_CASE(
        "a cancel of the awaited task still ends the awaiter as cancelled") {
        Runtime rt(2);
        std::atomic<bool> went_on{false};
        auto task = [&]() -> coro::CoroTask<IntResult> {
            const int v = co_await coro::or_fail(late(1, false));
            went_on.store(true);
            co_return v;
        };
        auto cancelled = task();
        cancelled.cancel();
        CHECK_THROWS(run(rt, std::move(cancelled)));
        CHECK(!went_on.load());
    }

    TEST_CASE("each await an error ends reports its end") {
        std::atomic<std::size_t> begins{0};
        std::atomic<std::size_t> ends{0};
        coro::CoroutineHooks hooks;
        hooks.user = nullptr;
        static std::atomic<std::size_t>* s_begins = nullptr;
        static std::atomic<std::size_t>* s_ends = nullptr;
        s_begins = &begins;
        s_ends = &ends;
        hooks.enqueue = [](void*, const void*, coro::CoroutineOrigin) {};
        hooks.resume = [](void*, const void*) {};
        hooks.suspend = [](void*, const void*) {};
        hooks.finish = [](void*, const void*) {};
        hooks.await_begin = [](void*, const void*) { ++*s_begins; };
        hooks.await_end = [](void*, const void*) { ++*s_ends; };
        coro::set_coroutine_hooks(&hooks);
        {
            Runtime rt(2);
            auto task = [](int i) -> coro::CoroTask<IntResult> {
                const int a = co_await coro::or_fail(now(i, i % 3 == 0));
                const int b = co_await coro::or_fail(late(i, i % 3 == 1));
                co_return a + b;
            };
            for (int i = 0; i < 60; ++i) (void)run(rt, task(i));
            rt.shutdown();
        }
        coro::set_coroutine_hooks(nullptr);
        CHECK(begins.load() > 0);
        CHECK(begins.load() == ends.load());
    }

    TEST_CASE("an exception of the awaited task still reaches the awaiter") {
        Runtime rt(2);
        auto thrower = []() -> coro::CoroTask<IntResult> {
            co_await coro::yield();
            throw std::runtime_error("boom");
        };
        auto task = [&]() -> coro::CoroTask<IntResult> {
            co_return co_await coro::or_fail(thrower());
        };
        CHECK_THROWS_AS(run(rt, task()), std::runtime_error);
    }
}
