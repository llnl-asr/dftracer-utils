// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/hooks.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

using namespace dftracer::utils;
using coro::CoroutineEvent;
using coro::CoroutineEventKind;

namespace {

struct Counts {
    std::atomic<std::size_t> enqueue{0};
    std::atomic<std::size_t> resume{0};
    std::atomic<std::size_t> suspend{0};
    std::atomic<std::size_t> finish{0};
    std::atomic<std::size_t> await_begin{0};
    std::atomic<std::size_t> await_end{0};
    std::atomic<std::size_t> off_worker{0};
};

coro::CoroutineHooks counting_hooks(Counts& counts) {
    coro::CoroutineHooks hooks;
    hooks.user = &counts;
    hooks.enqueue = [](void* user, const void*, coro::CoroutineOrigin) {
        ++static_cast<Counts*>(user)->enqueue;
    };
    hooks.resume = [](void* user, const void*) {
        auto* c = static_cast<Counts*>(user);
        ++c->resume;
        if (coro::current_worker_index() < 0) ++c->off_worker;
    };
    hooks.suspend = [](void* user, const void*) {
        ++static_cast<Counts*>(user)->suspend;
    };
    hooks.finish = [](void* user, const void*) {
        ++static_cast<Counts*>(user)->finish;
    };
    hooks.await_begin = [](void* user, const void*) {
        ++static_cast<Counts*>(user)->await_begin;
    };
    hooks.await_end = [](void* user, const void*) {
        ++static_cast<Counts*>(user)->await_end;
    };
    return hooks;
}

coro::CoroTask<void> yield_twice() {
    co_await coro::yield();
    co_await coro::yield();
}

coro::CoroTask<int> inline_child() { co_return 1; }

coro::CoroTask<void> await_inline(std::atomic<int>& sum) {
    const int value = co_await inline_child();
    sum.fetch_add(value);
}

}  // namespace

TEST_SUITE("CoroutineHooks") {
    TEST_CASE("each slice of a coroutine shows a resume and a suspend") {
        Counts counts;
        const coro::CoroutineHooks hooks = counting_hooks(counts);
        coro::set_coroutine_hooks(&hooks);
        {
            Runtime rt(2);
            constexpr std::size_t CHILDREN = 16;
            dftu_utils_test::ok(rt.block_on(coro::with_scope(
                [](coro::Scope& scope) -> coro::CoroTask<void> {
                    for (std::size_t i = 0; i < CHILDREN; ++i) {
                        scope.spawn(yield_twice());
                    }
                    co_return;
                })));
            rt.shutdown();
            coro::set_coroutine_hooks(nullptr);
            CHECK(counts.resume.load() == counts.suspend.load());
            CHECK(counts.finish.load() >= CHILDREN);
            // Each child: its spawn and two yields, each with three slices.
            CHECK(counts.resume.load() >= 3 * CHILDREN);
            CHECK(counts.enqueue.load() >= CHILDREN);
            CHECK(counts.off_worker.load() == 0);
        }
        coro::set_coroutine_hooks(nullptr);
    }

    TEST_CASE("a task run on the stack of its awaiter shows its await") {
        Counts counts;
        const coro::CoroutineHooks hooks = counting_hooks(counts);
        coro::set_coroutine_hooks(&hooks);
        Runtime rt(1);
        std::atomic<int> sum{0};
        dftu_utils_test::ok(rt.block_on(await_inline(sum)));
        rt.shutdown();
        coro::set_coroutine_hooks(nullptr);
        CHECK(sum.load() == 1);
        CHECK(counts.await_begin.load() >= 1);
        CHECK(counts.await_begin.load() == counts.await_end.load());
    }

    TEST_CASE("a removed table sees no more events on the same workers") {
        Counts counts;
        const coro::CoroutineHooks hooks = counting_hooks(counts);
        coro::set_coroutine_hooks(&hooks);
        Runtime rt(1);
        dftu_utils_test::ok(rt.block_on(yield_twice()));
        const std::size_t before_removal = counts.resume.load();
        CHECK(before_removal > 0);
        coro::set_coroutine_hooks(nullptr);
        // A slice that began before the removal may still report its suspend.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const std::size_t after_removal = counts.resume.load();
        dftu_utils_test::ok(rt.block_on(yield_twice()));
        rt.shutdown();
        CHECK(counts.resume.load() == after_removal);
        CHECK(coro::current_worker_index() == -1);
    }

    TEST_CASE("the event log gives each worker's events in order") {
        coro::CoroutineEventLog log;
        log.start();
        {
            Runtime rt(4);
            constexpr std::size_t CHILDREN = 200;
            dftu_utils_test::ok(rt.block_on(coro::with_scope(
                [](coro::Scope& scope) -> coro::CoroTask<void> {
                    for (std::size_t i = 0; i < CHILDREN; ++i) {
                        scope.spawn(yield_twice());
                    }
                    co_return;
                })));
            rt.shutdown();
        }
        log.stop();

        std::size_t resumes = 0;
        std::size_t ends = 0;
        std::size_t finishes = 0;
        std::size_t out_of_order = 0;
        dstd::HashMap<std::int32_t, std::int64_t> last;
        const std::size_t total = log.drain([&](const CoroutineEvent& event) {
            if (event.kind == CoroutineEventKind::Resume) ++resumes;
            if (event.kind == CoroutineEventKind::Suspend) ++ends;
            if (event.kind == CoroutineEventKind::Finish) ++finishes;
            if (event.worker < 0) return;
            auto [it, inserted] = last.emplace(event.worker, event.time_ns);
            if (!inserted) {
                if (event.time_ns < it->second) ++out_of_order;
                it->second = event.time_ns;
            }
        });
        CHECK(log.dropped() == 0);
        CHECK(total > 0);
        CHECK(resumes == ends);
        CHECK(finishes >= 200);
        CHECK(out_of_order == 0);
        CHECK(log.drain([](const CoroutineEvent&) {}) == 0);
    }

    TEST_CASE("a log that nobody reads drops and counts, and a read frees it") {
        auto burst = []() -> coro::CoroTask<void> {
            co_await coro::with_scope(
                [](coro::Scope& scope) -> coro::CoroTask<void> {
                    for (int i = 0; i < 100; ++i) scope.spawn(yield_twice());
                    co_return;
                });
        };
        coro::CoroutineEventLogOptions options;
        options.chunk_events = 8;
        options.chunks_per_worker = 2;
        coro::CoroutineEventLog log(options);
        log.start();
        Runtime rt(1);
        dftu_utils_test::ok(rt.block_on(burst()));
        CHECK(log.dropped() > 0);
        std::size_t seen = 0;
        log.drain([&](const CoroutineEvent&) { ++seen; });
        CHECK(seen > 0);

        dftu_utils_test::ok(rt.block_on(burst()));
        rt.shutdown();
        log.stop();
        std::size_t seen_after_read = 0;
        log.drain([&](const CoroutineEvent& event) {
            if (event.worker >= 0) ++seen_after_read;
        });
        CHECK(seen_after_read > 0);
    }

    TEST_CASE("a second log on the same workers sees only its own events") {
        coro::CoroutineEventLog first;
        coro::CoroutineEventLog second;
        Runtime rt(1);

        first.start();
        dftu_utils_test::ok(rt.block_on(yield_twice()));
        // The worker hands over its partial chunk when it goes idle.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        first.stop();
        std::size_t first_worker_events = 0;
        first.drain([&](const CoroutineEvent& event) {
            if (event.worker >= 0) ++first_worker_events;
        });
        dftu_utils_test::ok(rt.block_on(yield_twice()));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        second.start();
        dftu_utils_test::ok(rt.block_on(yield_twice()));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        second.stop();
        rt.shutdown();

        CHECK(first_worker_events > 0);
        CHECK(first.drain([](const CoroutineEvent&) {}) == 0);
        CHECK(second.drain([](const CoroutineEvent&) {}) > 0);
    }
}
