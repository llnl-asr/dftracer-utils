// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/support.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <thread>
#include <utility>

using namespace dftracer::utils;

namespace {

comm::Communicator make_communicator(std::uint32_t ranks,
                                     coro::Executor& exec) {
    auto started = comm::Communicator::start_local(ranks, exec);
    REQUIRE(started.has_value());
    return std::move(*started);
}

/// The frame a test drives by hand: awaiting the task is the only thing that
/// keeps it alive, so the driver releases its handle once it is set up.
coro::detail::Coro drive(coro::CoroTask<void> task, std::atomic<bool>& done,
                         dstd::Optional<dstd::Error>& error) {
    auto settled = co_await coro::settle(std::move(task));
    error = std::move(settled.error);
    done.store(true, std::memory_order_release);
}

}  // namespace

TEST_SUITE("CommOp") {
    TEST_CASE("test, wait and co_await give the outcome of one call") {
        Runtime rt(2);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        comm::Group world0 = communicator.world(0);
        comm::Op<void> call = world0.barrier();

        CHECK_FALSE(call.test());
        CHECK(call.status() == comm::OpStatus::Pending);

        std::atomic<bool> peer_ok{false};
        std::atomic<bool> peer_done{false};
        std::thread peer([&] {
            comm::Group world1 = communicator.world(1);
            comm::Op<void> other = world1.barrier();
            auto outcome = other.wait();
            peer_ok.store(outcome.has_value(), std::memory_order_release);
            peer_done.store(true, std::memory_order_release);
        });

        dftu_utils_test::ok(call.wait());
        CHECK(call.test());
        CHECK(call.status() == comm::OpStatus::Ok);
        dftu_utils_test::ok(call.wait());
        peer.join();
        CHECK(peer_done.load());
        CHECK(peer_ok.load());
    }

    TEST_CASE("a call awaited by both ranks ends once for both") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                comm::Op<void> call = group.barrier();
                auto awaited = co_await call;
                if (!awaited) co_return std::move(awaited).error();
                auto waited = call.wait();
                co_return waited;
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a cancel that races the completion leaves one outcome") {
        for (std::uint32_t round = 0; round < 200; ++round) {
            dftu_comm_test::QueueExecutor exec;
            comm::Communicator communicator = make_communicator(2, exec);
            comm::Group world0 = communicator.world(0);
            comm::Group world1 = communicator.world(1);
            comm::Op<void> call = world0.barrier();

            std::atomic<int> first_outcome{0};
            std::atomic<bool> peer_ok{false};
            std::atomic<bool> peer_second_ok{false};
            std::thread peer([&] {
                comm::Op<void> other = world1.barrier();
                auto outcome = other.wait();
                peer_ok.store(outcome.has_value(), std::memory_order_release);
                // The second call needs both ranks, so it follows what rank 0
                // saw: a group that a cancel aborted has no second call.
                dftu_comm_test::spin_until([&] {
                    return first_outcome.load(std::memory_order_acquire) != 0;
                });
                if (first_outcome.load(std::memory_order_acquire) != 1) return;
                comm::Op<void> again = world1.barrier();
                peer_second_ok.store(again.wait().has_value(),
                                     std::memory_order_release);
            });

            if (round % 2 == 0) call.cancel();
            auto first = call.wait();
            first_outcome.store(first.has_value() ? 1 : 2,
                                std::memory_order_release);

            // A call that ended Ok leaves the group healthy, so the next call
            // of both ranks must meet. A cancel of a finished call must not
            // abort it.
            bool rank0_second_ok = false;
            if (first.has_value()) {
                comm::Op<void> again = world0.barrier();
                rank0_second_ok = again.wait().has_value();
            }
            peer.join();
            CHECK(rank0_second_ok == first.has_value());
            CHECK(peer_second_ok.load() == first.has_value());
            CHECK(peer_ok.load() == first.has_value());

            // One outcome, and it does not change.
            CHECK(call.test());
            CHECK(call.status() != comm::OpStatus::Pending);
            auto second = call.wait();
            CHECK((call.status() == comm::OpStatus::Canceled) ==
                  !first.has_value());
            CHECK((call.status() == comm::OpStatus::Ok) == first.has_value());
            CHECK(first.has_value() == second.has_value());
            if (!first.has_value()) {
                CHECK(first.error().is(dstd::Condition::Cancelled));
                CHECK(dftu_comm_test::has_text(first.error(), "canceled"));
                CHECK(second.error().message() == first.error().message());
            }
        }
    }

    TEST_CASE("canceling a call that already ended keeps its outcome") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                comm::Op<void> call = group.barrier();
                auto first = co_await call;
                if (!first) co_return std::move(first).error();
                if (rank == 0) call.cancel();
                // The group was not aborted, so the next call ends for both.
                auto second = co_await group.barrier();
                co_return second;
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a cancel of the waiting task wakes it and aborts the group") {
        dftu_comm_test::QueueExecutor exec;
        comm::Communicator communicator = make_communicator(2, exec);
        comm::Group world0 = communicator.world(0);
        comm::Group world1 = communicator.world(1);

        dstd::Optional<comm::Op<void>> parked;
        std::atomic<bool> registered{false};
        std::atomic<bool> resumed{false};
        dstd::Optional<dstd::Error> woke_with;

        auto body = [&]() -> coro::detail::Coro {
            parked = world0.barrier();
            registered.store(true, std::memory_order_release);
            auto outcome = co_await *parked;
            if (!outcome) woke_with = std::move(outcome).error();
            resumed.store(true, std::memory_order_release);
        };
        auto frame = body();
        exec.post(frame.release());
        exec.run();
        CHECK(registered.load());
        CHECK_FALSE(resumed.load());
        CHECK(exec.parked() == 1);

        // What a shutdown does: every parked waiter is woken as cancelled.
        exec.cancel_all();
        CHECK_FALSE(resumed.load());
        exec.run();

        CHECK(resumed.load());
        CHECK(exec.parked() == 0);
        REQUIRE(woke_with.has_value());
        CHECK(woke_with->is(dstd::Condition::Cancelled));
        CHECK(dftu_comm_test::has_text(*woke_with, "aborted by rank 0"));
        REQUIRE(parked.has_value());
        auto ended = parked->wait();
        REQUIRE_FALSE(ended.has_value());
        CHECK(ended.error().message() == woke_with->message());

        // The peers end with the same error, on their pending and later calls.
        comm::Op<void> later = world1.barrier();
        auto later_outcome = later.wait();
        REQUIRE_FALSE(later_outcome.has_value());
        CHECK(later_outcome.error().message() == woke_with->message());
    }

    TEST_CASE("a woken task that holds no wait never resumes") {
        dftu_comm_test::QueueExecutor exec;
        comm::Communicator communicator = make_communicator(2, exec);
        comm::Group world0 = communicator.world(0);
        comm::Group world1 = communicator.world(1);

        dstd::Optional<comm::Op<void>> parked;
        std::atomic<bool> registered{false};
        std::atomic<bool> body_end{false};
        std::atomic<bool> driver_done{false};
        dstd::Optional<dstd::Error> driver_error;

        auto body = [&]() -> coro::CoroTask<void> {
            parked = world0.barrier();
            registered.store(true, std::memory_order_release);
            auto outcome = co_await *parked;
            (void)outcome;
            body_end.store(true, std::memory_order_release);
        };
        auto driver = drive(body(), driver_done, driver_error);
        exec.post(driver.release());
        exec.run();
        CHECK(registered.load());
        CHECK(exec.parked() == 1);

        exec.cancel_all();
        exec.run();

        CHECK(driver_done.load());
        CHECK_FALSE(body_end.load());
        CHECK(exec.parked() == 0);
        REQUIRE(parked.has_value());
        CHECK(parked->test());
        auto ended = parked->wait();
        REQUIRE_FALSE(ended.has_value());
        CHECK(ended.error().is(dstd::Condition::Cancelled));

        comm::Op<void> later = world1.barrier();
        auto later_outcome = later.wait();
        REQUIRE_FALSE(later_outcome.has_value());
        CHECK(later_outcome.error().message() == ended.error().message());
    }

    TEST_CASE("a stopped scope leaves a barrier that will never complete") {
        Runtime rt(2);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> registered{false};
        std::atomic<bool> child_end{false};

        auto driver =
            coro::with_scope([&communicator, &registered, &child_end](
                                 coro::Scope& scope) -> coro::CoroTask<void> {
                scope.spawn([&communicator, &registered,
                             &child_end]() -> coro::CoroTask<void> {
                    comm::Group group = communicator.world(0);
                    comm::Op<void> call = group.barrier();
                    registered.store(true, std::memory_order_release);
                    auto outcome = co_await call;
                    (void)outcome;
                    child_end.store(true, std::memory_order_release);
                });
                co_await dftu_comm_test::wait_flag(registered);
                co_await coro::yield();
                scope.request_stop();
                co_return;
            });
        dftu_utils_test::ok(rt.block_on(std::move(driver)));
        CHECK_FALSE(child_end.load());

        // The stop aborted the group, so a peer's later call says so.
        comm::Group world1 = communicator.world(1);
        comm::Op<void> later = world1.barrier();
        auto later_outcome = later.wait();
        REQUIRE_FALSE(later_outcome.has_value());
        CHECK(later_outcome.error().is(dstd::Condition::Cancelled));
        CHECK(dftu_comm_test::has_text(later_outcome.error(),
                                       "aborted by rank 0"));
    }
}
