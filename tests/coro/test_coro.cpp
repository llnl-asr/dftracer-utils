#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/coro/join_handle.h>
#include <dftracer/utils/core/coro/yield.h>
#include <doctest/doctest.h>

#include <coroutine>
#include <stdexcept>
#include <vector>

using namespace dftracer::utils::coro;

// Helper: coroutine lambdas MUST be named (not temporaries) because
// the lambda object holds the captures and must outlive the coroutine
// frame. A temporary lambda is destroyed after initial_suspend,
// leaving a dangling pointer in the coroutine frame.

// ============================================================================
// Coro - Basic Tests
// ============================================================================

TEST_CASE("Coro - Construction and destruction without running") {
    auto make = []() -> Coro { co_return; };
    auto c = make();

    CHECK(c.handle() != nullptr);
    CHECK(c.done() == false);  // Not started (initial_suspend = always)
}

TEST_CASE("Coro - Resume manually, completes") {
    bool executed = false;
    auto* ptr = &executed;
    auto make = [ptr]() -> Coro {
        *ptr = true;
        co_return;
    };
    auto c = make();

    CHECK(executed == false);
    c.handle().resume();  // Start (past initial_suspend)
    CHECK(executed == true);
    // Now at final_suspend -- done
    CHECK(c.handle().done() == true);
}

TEST_CASE("Coro - Exception captured in promise") {
    auto make = []() -> Coro {
        throw std::runtime_error("coro error");
        co_return;
    };
    auto c = make();

    c.handle().resume();
    CHECK(c.handle().done() == true);

    auto& promise = c.handle().promise();
    CHECK(promise.exception != nullptr);

    bool caught = false;
    try {
        std::rethrow_exception(promise.exception);
    } catch (const std::runtime_error& e) {
        caught = true;
        CHECK(std::string(e.what()) == "coro error");
    }
    CHECK(caught == true);
}

TEST_CASE("Coro - Move semantics") {
    auto make = []() -> Coro { co_return; };
    auto c1 = make();

    auto handle = c1.handle();
    CHECK(handle != nullptr);

    // Move construct
    Coro c2(std::move(c1));
    CHECK(c2.handle() == handle);
    CHECK(c1.handle() == nullptr);  // NOLINT(bugprone-use-after-move)
    CHECK(c1.done() == false);      // NOLINT(bugprone-use-after-move)

    // Move assign
    auto make2 = []() -> Coro { co_return; };
    auto c3 = make2();
    c3 = std::move(c2);
    CHECK(c3.handle() == handle);
    CHECK(c2.handle() == nullptr);  // NOLINT(bugprone-use-after-move)
}

TEST_CASE("Coro - Release transfers ownership") {
    auto make = []() -> Coro { co_return; };
    auto c = make();

    auto handle = c.release();
    CHECK(handle != nullptr);
    CHECK(c.handle() == nullptr);

    // We own it now -- must destroy manually
    handle.resume();  // run to completion
    CHECK(handle.done() == true);
    handle.destroy();
}

TEST_CASE("Coro - FinalAwaiter returns noop when no join group") {
    auto make = []() -> Coro { co_return; };
    auto c = make();

    // Resume to completion -- FinalAwaiter should return noop_coroutine
    // since no join_counter is set.
    c.handle().resume();
    CHECK(c.handle().done() == true);

    auto& promise = c.handle().promise();
    CHECK(promise.join_counter == nullptr);
    CHECK(promise.exception == nullptr);
}

TEST_CASE("Coro - Multiple suspension points") {
    int stage = 0;
    auto* stage_ptr = &stage;
    auto make = [stage_ptr]() -> Coro {
        *stage_ptr = 1;
        co_await yield();
        *stage_ptr = 2;
        co_await yield();
        *stage_ptr = 3;
        co_return;
    };
    auto c = make();

    CHECK(stage == 0);
    c.handle().resume();  // past initial_suspend
    CHECK(stage == 1);
    c.handle().resume();  // past first yield
    CHECK(stage == 2);
    c.handle().resume();  // past second yield
    CHECK(stage == 3);
    CHECK(c.handle().done() == true);
}

// ============================================================================
// JoinHandle - Basic Tests
// ============================================================================

TEST_CASE("JoinHandle - Zero coros, join completes immediately") {
    JoinHandle jh;
    // pending_ starts at 1 (joiner's pre-allocated slot)
    CHECK(jh.pending() == 1);

    // Join always suspends (await_ready returns false),
    // but await_suspend sees prev==1 and resumes inline.
    auto awaitable = jh.join();
    CHECK(awaitable.await_ready() == false);
}

TEST_CASE("JoinHandle - One coro, waits then completes") {
    JoinHandle jh;

    auto make = []() -> Coro { co_return; };
    auto c = make();
    jh.track(c);

    CHECK(jh.pending() == 2);  // 1 coro + 1 joiner slot

    // Join should NOT be ready
    auto awaitable = jh.join();
    CHECK(awaitable.await_ready() == false);

    // Complete the coro
    c.handle().resume();  // Run to final_suspend

    // Counter should be 1 (joiner's slot remains)
    CHECK(jh.pending() == 1);
}

TEST_CASE("JoinHandle - N coros, completes when last finishes") {
    JoinHandle jh;

    std::vector<Coro> coros;
    // Must keep lambdas alive -- but these are stateless, so temporaries
    // would be fine. However, for consistency, use a named maker.
    auto make = []() -> Coro { co_return; };
    for (int i = 0; i < 5; ++i) {
        coros.push_back(make());
        jh.track(coros.back());
    }

    CHECK(jh.pending() == 6);  // 5 coros + 1 joiner slot

    // Complete all but last
    for (int i = 0; i < 4; ++i) {
        coros[i].handle().resume();
        CHECK(jh.pending() == static_cast<std::size_t>(5 - i));
    }

    CHECK(jh.pending() == 2);  // 1 coro left + 1 joiner slot

    // Complete last
    coros[4].handle().resume();
    CHECK(jh.pending() == 1);  // joiner's slot remains
}

TEST_CASE("JoinHandle - Coro with exception, join still completes") {
    JoinHandle jh;

    auto make = []() -> Coro {
        throw std::runtime_error("error");
        co_return;
    };
    auto c = make();
    jh.track(c);

    CHECK(jh.pending() == 2);  // 1 coro + 1 joiner slot
    c.handle().resume();       // Throws, caught by promise
    CHECK(jh.pending() == 1);  // joiner's slot remains

    // Exception stored in coro, not propagated to join
    auto& promise = c.handle().promise();
    CHECK(promise.exception != nullptr);
}

TEST_CASE("JoinHandle - FinalAwaiter resumes continuation on last coro") {
    // This tests the symmetric transfer path where the last coro
    // in a join group resumes the joiner coroutine.

    JoinHandle jh;
    bool joiner_resumed = false;
    auto* resumed_ptr = &joiner_resumed;

    // Create a worker coro
    auto make_worker = []() -> Coro { co_return; };
    auto worker = make_worker();
    jh.track(worker);

    // Create a "joiner" coroutine that co_awaits the join handle
    auto make_joiner = [&jh, resumed_ptr]() -> Coro {
        co_await jh.join();
        *resumed_ptr = true;
        co_return;
    };
    auto joiner = make_joiner();

    // Start the joiner -- it will suspend at jh.join()
    joiner.handle().resume();
    CHECK(joiner_resumed == false);

    // Complete the worker -- FinalAwaiter should resume joiner
    // via symmetric transfer
    worker.handle().resume();

    CHECK(jh.pending() == 0);  // joiner's await_suspend also decremented
    CHECK(joiner_resumed == true);
    CHECK(joiner.handle().done() == true);
}

TEST_CASE(
    "JoinHandle - Multiple coros with joiner, "
    "resumes after all complete") {
    JoinHandle jh;
    bool joiner_resumed = false;
    auto* resumed_ptr = &joiner_resumed;

    auto make_worker = []() -> Coro { co_return; };
    auto w1 = make_worker();
    auto w2 = make_worker();
    auto w3 = make_worker();
    jh.track(w1);
    jh.track(w2);
    jh.track(w3);

    auto make_joiner = [&jh, resumed_ptr]() -> Coro {
        co_await jh.join();
        *resumed_ptr = true;
        co_return;
    };
    auto joiner = make_joiner();

    // Start joiner
    joiner.handle().resume();
    CHECK(joiner_resumed == false);

    // Complete first two workers -- joiner should NOT resume yet
    w1.handle().resume();
    CHECK(joiner_resumed == false);
    CHECK(jh.pending() ==
          2);  // 2 workers left (joiner's slot consumed by await_suspend)

    w2.handle().resume();
    CHECK(joiner_resumed == false);
    CHECK(jh.pending() == 1);  // 1 worker left

    // Complete last worker -- joiner should resume
    w3.handle().resume();
    CHECK(joiner_resumed == true);
    CHECK(jh.pending() == 0);  // all done
}

TEST_CASE("JoinHandle - All coros complete before join() is called") {
    JoinHandle jh;

    auto make = []() -> Coro { co_return; };
    auto c = make();
    jh.track(c);
    c.handle().resume();

    // After coro completes: pending = 2 (track+1) - 1 (coro) = 1 (joiner slot)
    CHECK(jh.pending() == 1);

    // await_ready always returns false (must go through await_suspend
    // to decrement the joiner's slot)
    auto awaitable = jh.join();
    CHECK(awaitable.await_ready() == false);
}

// ============================================================================
// Yield - Basic Tests
// ============================================================================

TEST_CASE("Yield - Suspends coroutine") {
    int stage = 0;
    auto* stage_ptr = &stage;
    auto make = [stage_ptr]() -> Coro {
        *stage_ptr = 1;
        co_await yield();
        *stage_ptr = 2;
        co_return;
    };
    auto c = make();

    CHECK(stage == 0);
    c.handle().resume();  // past initial_suspend
    CHECK(stage == 1);

    // Coroutine is now suspended at yield()
    CHECK(c.handle().done() == false);

    // Manually resume (simulating executor re-enqueue)
    c.handle().resume();
    CHECK(stage == 2);
    CHECK(c.handle().done() == true);
}

TEST_CASE("Yield - Without executor returns noop") {
    auto make = []() -> Coro {
        co_await yield();
        co_return;
    };
    auto c = make();

    c.handle().resume();  // past initial_suspend, hits yield
    CHECK(c.handle().done() == false);

    c.handle().resume();  // past yield
    CHECK(c.handle().done() == true);
}

TEST_CASE("Yield - Multiple yields") {
    int count = 0;
    auto* count_ptr = &count;
    auto make = [count_ptr]() -> Coro {
        for (int i = 0; i < 5; ++i) {
            (*count_ptr)++;
            co_await yield();
        }
        co_return;
    };
    auto c = make();

    // initial_suspend + 5 yields = 6 resumes to reach final
    for (int i = 0; i < 5; ++i) {
        c.handle().resume();
        CHECK(count == i + 1);
    }

    CHECK(c.handle().done() == false);
    c.handle().resume();  // final co_return
    CHECK(c.handle().done() == true);
    CHECK(count == 5);
}

// ============================================================================
// Integration: Coro + JoinHandle + Yield
// ============================================================================

TEST_CASE("Integration - JoinHandle with yielding coros") {
    JoinHandle jh;
    int w1_stage = 0;
    int w2_stage = 0;
    bool joiner_done = false;

    auto* w1_ptr = &w1_stage;
    auto* w2_ptr = &w2_stage;
    auto* joiner_ptr = &joiner_done;

    auto make_worker1 = [w1_ptr]() -> Coro {
        *w1_ptr = 1;
        co_await yield();
        *w1_ptr = 2;
        co_return;
    };

    auto make_worker2 = [w2_ptr]() -> Coro {
        *w2_ptr = 1;
        co_await yield();
        *w2_ptr = 2;
        co_return;
    };

    auto worker1 = make_worker1();
    auto worker2 = make_worker2();

    jh.track(worker1);
    jh.track(worker2);

    auto make_joiner = [&jh, joiner_ptr]() -> Coro {
        co_await jh.join();
        *joiner_ptr = true;
        co_return;
    };
    auto joiner = make_joiner();

    // Start joiner -- suspends at join()
    joiner.handle().resume();
    CHECK(joiner_done == false);

    // Start workers -- they each reach their yield point
    worker1.handle().resume();
    worker2.handle().resume();
    CHECK(w1_stage == 1);
    CHECK(w2_stage == 1);
    CHECK(joiner_done == false);

    // Resume worker1 past yield -- it completes
    worker1.handle().resume();
    CHECK(w1_stage == 2);
    CHECK(joiner_done == false);  // worker2 still pending

    // Resume worker2 past yield -- it completes, triggering join
    worker2.handle().resume();
    CHECK(w2_stage == 2);
    CHECK(joiner_done == true);
    CHECK(jh.pending() == 0);
}
