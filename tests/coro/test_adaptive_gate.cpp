#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/coro/adaptive_gate.h>
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/coro/when_all.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using dftu_utils_test::run_coro;

namespace {

constexpr std::uint64_t BASE_NS = 300000;

// Time per operation of a metadata server that serves `knee` calls at once at
// full speed and queues the rest, so each call slows in step with the excess.
std::uint64_t contended_ns(std::size_t concurrency, std::size_t knee) {
    if (concurrency <= knee) return BASE_NS;
    return BASE_NS * concurrency / knee;
}

std::size_t settle(coro::AdaptiveLimit& limit, std::size_t knee, int units) {
    for (int i = 0; i < units; ++i)
        limit.record(contended_ns(limit.limit(), knee), true);
    return limit.limit();
}

}  // namespace

TEST_SUITE("AdaptiveLimit") {
    TEST_CASE("constant latency grows the limit to the cap") {
        coro::AdaptiveLimit limit(48);
        CHECK(limit.limit() == coro::ADAPTIVE_INITIAL_LIMIT);
        for (int i = 0; i < 200; ++i) limit.record(BASE_NS, true);
        CHECK(limit.limit() == 48);
    }

    TEST_CASE("a gate that is not full does not grow") {
        coro::AdaptiveLimit limit(48);
        for (int i = 0; i < 200; ++i) limit.record(BASE_NS, false);
        CHECK(limit.limit() == coro::ADAPTIVE_INITIAL_LIMIT);
    }

    TEST_CASE("latency that rises past a knee holds the limit near it") {
        for (const std::size_t knee : {2, 6, 12, 20}) {
            coro::AdaptiveLimit limit(48);
            settle(limit, knee, 100);
            std::size_t low = limit.cap();
            std::size_t high = 0;
            for (int i = 0; i < 2000; ++i) {
                limit.record(contended_ns(limit.limit(), knee), true);
                low = std::min(low, limit.limit());
                high = std::max(high, limit.limit());
            }
            INFO("knee " << knee << " low " << low << " high " << high);
            CHECK(high <= 2 * knee + 1);
            CHECK(low * 4 >= knee * 2);
            CHECK(high < limit.cap());
        }
    }

    TEST_CASE("one slow unit cuts the limit once per window") {
        coro::AdaptiveLimit limit(48);
        for (int i = 0; i < 100; ++i) limit.record(BASE_NS, true);
        REQUIRE(limit.limit() == 48);
        for (int i = 0; i < 36; ++i) limit.record(BASE_NS * 10, true);
        CHECK(limit.limit() == 36);
    }

    TEST_CASE("load that stays for the whole run is probed again from one") {
        coro::AdaptiveLimit limit(8);
        limit.record(BASE_NS, true);
        for (int i = 0; i < 100; ++i) limit.record(BASE_NS * 10, true);
        CHECK(limit.limit() > 1);
        for (int i = 0; i < 100; ++i) limit.record(BASE_NS * 10, true);
        CHECK(limit.limit() == 8);
    }

    TEST_CASE("a cap of zero still admits one") {
        coro::AdaptiveLimit limit(0);
        CHECK(limit.cap() == 1);
        CHECK(limit.limit() == 1);
    }
}

namespace {

struct Concurrency {
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
};

// A unit that holds a ticket across a blocking call of `t`, as a stat batch
// does. A named coroutine with by-value parameters, not a capturing lambda.
coro::CoroTask<int> gated_unit(coro::AdaptiveGate& gate, Concurrency& c, int i,
                               std::chrono::microseconds t) {
    auto admitted = co_await gate.enter(1);
    const int now = c.running.fetch_add(1) + 1;
    int seen = c.peak.load();
    while (now > seen && !c.peak.compare_exchange_weak(seen, now)) {
    }
    std::this_thread::sleep_for(t);
    c.running.fetch_sub(1);
    co_return i;
}

coro::CoroTask<int> throwing_unit(coro::AdaptiveGate& gate) {
    auto admitted = co_await gate.enter(1);
    throw std::runtime_error("stat failed");
}

}  // namespace

TEST_SUITE("AdaptiveGate") {
    TEST_CASE("runs every unit and never more than the cap at once") {
        coro::AdaptiveGate gate(8);
        Concurrency c;
        const int n = 64;

        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            std::vector<coro::CoroTask<int>> tasks;
            for (int i = 0; i < n; ++i)
                tasks.push_back(
                    gated_unit(gate, c, i, std::chrono::microseconds(200)));
            const auto results = co_await coro::when_all(std::move(tasks));
            for (int i = 0; i < n; ++i) CHECK(results[i] == i);
            co_return;
        });

        CHECK(c.running.load() == 0);
        CHECK(c.peak.load() >= 1);
        CHECK(c.peak.load() <= 8);
    }

    TEST_CASE("units keep running after slow ones cut the limit") {
        coro::AdaptiveGate gate(8);
        Concurrency c;
        const int phase = 24;
        std::size_t lowest = 8;

        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            for (const auto t : {std::chrono::microseconds(100),
                                 std::chrono::microseconds(5000),
                                 std::chrono::microseconds(100)}) {
                std::vector<coro::CoroTask<int>> tasks;
                for (int i = 0; i < phase; ++i)
                    tasks.push_back(gated_unit(gate, c, i, t));
                const auto results = co_await coro::when_all(std::move(tasks));
                for (int i = 0; i < phase; ++i) CHECK(results[i] == i);
                lowest = std::min(lowest, gate.limit());
            }
            co_return;
        });

        CHECK(c.running.load() == 0);
        CHECK(c.peak.load() <= 8);
        CHECK(lowest < 8);
        CHECK(gate.limit() >= 1);
        CHECK(gate.limit() <= 8);
    }

    TEST_CASE("a unit that throws gives its slot back") {
        coro::AdaptiveGate gate(1);
        Concurrency c;
        int after = 0;

        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            bool threw = false;
            try {
                co_await throwing_unit(gate);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            CHECK(threw);
            after =
                co_await gated_unit(gate, c, 7, std::chrono::microseconds(1));
            co_return;
        });

        CHECK(after == 7);
    }
}
