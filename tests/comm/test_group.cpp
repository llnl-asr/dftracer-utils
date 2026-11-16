// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/support.h>
#include <dftracer/utils/comm/detail/group_state.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

using namespace dftracer::utils;

namespace {

constexpr std::uint64_t SOON_NS = 40'000'000;

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

std::atomic<bool> g_gate_hit{false};
std::atomic<bool> g_gate_release{false};

/// Holds the timer thread in the comm timeout path until the test lets go.
void test_gate() noexcept {
    g_gate_hit.store(true, std::memory_order_release);
    while (!g_gate_release.load(std::memory_order_acquire))
        std::this_thread::yield();
}

/// Clears the gate on every path out of a test case.
struct GateGuard {
    ~GateGuard() {
        g_gate_release.store(true, std::memory_order_release);
        comm::detail::Hooks::set_expire_gate(nullptr);
    }
};

}  // namespace

TEST_SUITE("CommGroup") {
    TEST_CASE("a barrier of two ranks ends for both") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        CHECK(communicator.world(0).rank() == 0);
        CHECK(communicator.world(1).rank() == 1);
        CHECK(communicator.world(0).size() == 2);
        CHECK(communicator.world(1).id() == communicator.world(0).id());

        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto outcome = co_await group.barrier();
                co_return outcome;
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("local has the members of world, and no rank is started empty") {
        Runtime rt(2);
        auto refused = comm::Communicator::start_local(0, *rt.executor());
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().is(dstd::Condition::InvalidArgument));

        comm::Communicator communicator = make_communicator(3, *rt.executor());
        CHECK(communicator.size() == 3);
        for (std::uint32_t rank = 0; rank < 3; ++rank) {
            comm::Group world = communicator.world(rank);
            comm::Group local = communicator.local(rank);
            CHECK(local.rank() == world.rank());
            CHECK(local.size() == world.size());
            CHECK(local.id() == world.id());
        }
    }

    TEST_CASE("a group of one rank ends its collectives at once") {
        Runtime rt(2);
        comm::Communicator communicator = make_communicator(1, *rt.executor());
        comm::Group group = communicator.world(0);
        CHECK(group.size() == 1);

        comm::Op<void> call = group.barrier();
        CHECK(call.test());
        dftu_utils_test::ok(call.wait());
        comm::Op<void> monitored = group.monitored_barrier();
        CHECK(monitored.test());
        dftu_utils_test::ok(monitored.wait());

        comm::Op<comm::Group> split = group.split(3, 0);
        CHECK(split.test());
        auto child = split.wait();
        REQUIRE(child.has_value());
        CHECK(child->size() == 1);
        CHECK(child->rank() == 0);
        CHECK(child->id() != group.id());
    }

    TEST_CASE("a rank that calls another collective is an error, not a wait") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 0) {
                    auto outcome = co_await group.barrier();
                    co_return outcome;
                }
                auto child = co_await group.split(1, 0);
                if (!child) co_return std::move(child).error();
                co_return {};
            });
        REQUIRE(errors[0].has_value());
        REQUIRE(errors[1].has_value());
        CHECK(errors[0]->is(dstd::Condition::InvalidArgument));
        CHECK(errors[0]->message() == errors[1]->message());
        CHECK(dftu_comm_test::has_text(*errors[0], "barrier"));
        CHECK(dftu_comm_test::has_text(*errors[0], "split"));
        CHECK(dftu_comm_test::has_text(*errors[0], "rank 0"));
        CHECK(dftu_comm_test::has_text(*errors[0], "rank 1"));
        CHECK(dftu_comm_test::count_text(*errors[0], "group ") == 1);
    }

    TEST_CASE("an abort fails the pending and the later calls of every rank") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(3, *rt.executor());
        std::atomic<int> arrived{0};
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator, &arrived](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 2) {
                    comm::Op<void> call = group.barrier();
                    arrived.fetch_add(1, std::memory_order_release);
                    auto pending = co_await call;
                    co_return pending;
                }
                co_await dftu_comm_test::wait_count(arrived, 1);
                if (rank == 1) {
                    co_await coro::yield();
                    group.abort(dstd::Error{dstd::Condition::Internal,
                                            "rank 1 gives up"});
                }
                // Every rank, the one that aborted too, sees the abort on a
                // call made after it.
                auto later = co_await group.barrier();
                co_return later;
            });
        for (std::uint32_t rank = 0; rank < 3; ++rank) {
            REQUIRE(errors[rank].has_value());
            CHECK(dftu_comm_test::has_text(*errors[rank], "aborted by rank 1"));
            CHECK(dftu_comm_test::has_text(*errors[rank], "rank 1 gives up"));
            CHECK(errors[rank]->message() == errors[0]->message());
        }
    }

    TEST_CASE("a deadline that passes on one rank aborts the group") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> finished{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &finished](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 0) {
                    comm::CallOpts opts;
                    opts.deadline_ns = dftu_comm_test::deadline_after(SOON_NS);
                    auto late = co_await group.barrier(opts);
                    finished.store(true, std::memory_order_release);
                    co_return late;
                }
                co_await dftu_comm_test::wait_flag(finished);
                auto later = co_await group.barrier();
                co_return later;
            });
        REQUIRE(errors[0].has_value());
        CHECK(errors[0]->is(dstd::Condition::Timeout));
        CHECK(dftu_comm_test::has_text(*errors[0], "deadline"));
        CHECK(dftu_comm_test::count_text(*errors[0], "group ") == 1);
        REQUIRE(errors[1].has_value());
        CHECK(errors[1]->message() == errors[0]->message());
    }

    TEST_CASE(
        "the default deadline of a group applies to a call that names none") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> finished{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &finished](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 0) {
                    group.set_default_deadline(SOON_NS);
                    auto late = co_await group.barrier();
                    finished.store(true, std::memory_order_release);
                    co_return late;
                }
                co_await dftu_comm_test::wait_flag(finished);
                auto later = co_await group.barrier();
                co_return later;
            });
        REQUIRE(errors[0].has_value());
        CHECK(errors[0]->is(dstd::Condition::Timeout));
        REQUIRE(errors[1].has_value());
        CHECK(errors[1]->message() == errors[0]->message());
    }

    TEST_CASE("the default deadline counts from each call, not from the set") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                group.set_default_deadline(250'000'000);
                // Five rounds of about 100 ms: each round is inside the 250 ms
                // timeout of its call, and the five are not. A timeout counted
                // from the set time would pass during the third round.
                for (int round = 0; round < 5; ++round) {
                    if (rank == 1) {
                        co_await coro::sleep(std::chrono::milliseconds(100));
                    }
                    auto outcome = co_await group.barrier();
                    if (!outcome) co_return std::move(outcome).error();
                }
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a monitored barrier names the ranks that did not arrive") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(3, *rt.executor());
        std::atomic<bool> finished{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator, &finished](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 0) {
                    comm::CallOpts opts;
                    opts.deadline_ns = dftu_comm_test::deadline_after(SOON_NS);
                    auto late = co_await group.monitored_barrier(opts);
                    finished.store(true, std::memory_order_release);
                    co_return late;
                }
                co_await dftu_comm_test::wait_flag(finished);
                auto later = co_await group.barrier();
                co_return later;
            });
        REQUIRE(errors[0].has_value());
        CHECK(errors[0]->is(dstd::Condition::Timeout));
        CHECK(dftu_comm_test::has_text(*errors[0], "missing ranks: 1, 2"));
        REQUIRE(errors[1].has_value());
        CHECK(errors[1]->message() == errors[0]->message());
        CHECK(errors[2]->message() == errors[0]->message());
    }

    TEST_CASE("canceling a collective aborts the group") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(3, *rt.executor());
        std::atomic<int> arrived{0};
        std::atomic<bool> canceled{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator, &arrived, &canceled](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 2) {
                    // A call made after the cancel carries the same error.
                    co_await dftu_comm_test::wait_flag(canceled);
                    auto later = co_await group.barrier();
                    co_return later;
                }
                if (rank == 1) {
                    comm::Op<void> call = group.barrier();
                    arrived.store(1, std::memory_order_release);
                    auto pending = co_await call;
                    co_return pending;
                }
                // A peer matched this call, so a local cancel must abort the
                // group instead of failing this call alone.
                co_await dftu_comm_test::wait_count(arrived, 1);
                co_await coro::yield();
                comm::Op<void> call = group.barrier();
                call.cancel();
                canceled.store(true, std::memory_order_release);
                if (!call.test())
                    co_return dstd::Error{dstd::Condition::Internal,
                                          "a cancel did not end the call"};
                co_return call.wait();
            });
        REQUIRE(errors[0].has_value());
        CHECK(errors[0]->is(dstd::Condition::Cancelled));
        CHECK(dftu_comm_test::has_text(*errors[0], "aborted by rank 0"));
        REQUIRE(errors[1].has_value());
        REQUIRE(errors[2].has_value());
        CHECK(errors[1]->message() == errors[0]->message());
        CHECK(errors[2]->message() == errors[0]->message());
    }

    TEST_CASE("a completion is posted, never run by the thread that made it") {
        dftu_comm_test::QueueExecutor exec;
        comm::Communicator communicator = make_communicator(2, exec);
        std::atomic<bool> resumed{false};
        std::atomic<bool> driver_done{false};
        dstd::Optional<dstd::Error> rank_error;
        dstd::Optional<dstd::Error> driver_error;

        auto rank0 = [&communicator, &resumed,
                      &rank_error]() -> coro::CoroTask<void> {
            comm::Group group = communicator.world(0);
            comm::Op<void> call = group.barrier();
            auto outcome = co_await call;
            if (!outcome) rank_error = std::move(outcome).error();
            resumed.store(true, std::memory_order_release);
            co_return;
        };
        auto task = drive(rank0(), driver_done, driver_error);
        exec.post(task.release());
        exec.run();
        CHECK_FALSE(resumed.load());
        CHECK(exec.queued() == 0);

        std::atomic<bool> peer_done{false};
        dstd::Optional<dstd::Error> peer_error;
        std::thread peer([&] {
            comm::Group group = communicator.world(1);
            comm::Op<void> call = group.barrier();
            auto outcome = call.wait();
            if (!outcome) peer_error = std::move(outcome).error();
            peer_done.store(true, std::memory_order_release);
        });
        peer.join();

        CHECK(peer_done.load());
        CHECK_FALSE(peer_error.has_value());
        // The peer's thread queued rank 0 instead of running it there.
        CHECK_FALSE(resumed.load());
        CHECK(exec.queued() == 1);

        exec.run();
        CHECK(resumed.load());
        CHECK(driver_done.load());
        CHECK_FALSE(rank_error.has_value());
        CHECK_FALSE(driver_error.has_value());
    }

    TEST_CASE("a deadline that fires as the call completes aborts nothing") {
        GateGuard guard;
        g_gate_hit.store(false, std::memory_order_release);
        g_gate_release.store(false, std::memory_order_release);
        comm::detail::Hooks::reset_expire_signal();
        comm::detail::Hooks::set_expire_gate(&test_gate);

        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        comm::Group world0 = communicator.world(0);
        comm::Group world1 = communicator.world(1);

        comm::CallOpts opts;
        opts.deadline_ns = dftu_comm_test::deadline_after(20'000'000);
        comm::Op<void> call = world0.barrier(opts);

        std::atomic<bool> peer_ok{false};
        std::thread peer([&] {
            // The peer arrives while the timer holds the group, so the call
            // completes after the timeout started.
            while (!g_gate_hit.load(std::memory_order_acquire))
                std::this_thread::yield();
            comm::Op<void> other = world1.barrier();
            peer_ok.store(other.wait().has_value(), std::memory_order_release);
        });
        REQUIRE(dftu_comm_test::spin_until(
            [] { return g_gate_hit.load(std::memory_order_acquire); }));
        auto first = call.wait();
        g_gate_release.store(true, std::memory_order_release);
        peer.join();
        // The timeout took its decision before the next calls, or not at all.
        REQUIRE(dftu_comm_test::spin_until(
            [] { return comm::detail::Hooks::expire_returned(); }));
        CHECK(comm::detail::Hooks::expire_at_lock());
        CHECK(first.has_value());
        CHECK(peer_ok.load());
        // The call ended before the timeout looked again, so the group is
        // whole and its next call completes.
        comm::Op<void> again0 = world0.barrier();
        comm::Op<void> again1 = world1.barrier();
        CHECK(again0.wait().has_value());
        CHECK(again1.wait().has_value());
    }

    TEST_CASE("a finished call leaves no state behind") {
        Runtime rt(4);
        const std::size_t ops_before = comm::detail::Hooks::live_ops();
        const std::size_t groups_before = comm::detail::Hooks::live_groups();
        {
            comm::Communicator communicator =
                make_communicator(2, *rt.executor());
            CHECK(comm::detail::Hooks::live_groups() == groups_before + 1);
            // A default deadline far in the future: its timers are still
            // registered when the calls end, and must pin nothing.
            auto errors = dftu_comm_test::run_ranks(
                rt, 2,
                [&communicator](
                    std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    group.set_default_deadline(10'000'000'000);
                    for (int round = 0; round < 50; ++round) {
                        auto outcome = co_await group.barrier();
                        if (!outcome) co_return std::move(outcome).error();
                    }
                    co_return {};
                });
            CHECK_FALSE(errors[0].has_value());
            CHECK_FALSE(errors[1].has_value());
            CHECK(comm::detail::Hooks::live_ops() == ops_before);
        }
        CHECK(comm::detail::Hooks::live_ops() == ops_before);
        CHECK(comm::detail::Hooks::live_groups() == groups_before);
    }

    TEST_CASE("a finished call frees its blocks, not at its deadline") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        constexpr int ROUNDS = 240;
        auto loop = [&communicator, &rt]() {
            return dftu_comm_test::run_ranks(
                rt, 2,
                [&communicator](
                    std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    // A deadline far in the future: what a timeout that is
                    // still pending holds.
                    group.set_default_deadline(3'600'000'000'000);
                    for (int round = 0; round < ROUNDS; ++round) {
                        auto outcome = co_await group.barrier();
                        if (!outcome) co_return std::move(outcome).error();
                    }
                    co_return {};
                });
        };
        // One loop first, so the pools and the timer queue have warmed up.
        auto warm = loop();
        CHECK_FALSE(warm[0].has_value());
        CHECK_FALSE(warm[1].has_value());
        const std::size_t before = memory::detail::Manager::get().stats().live;
        auto errors = loop();
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
        const std::size_t after = memory::detail::Manager::get().stats().live;
        const std::size_t kept = after > before ? after - before : 0;
        MESSAGE("kept " << kept << " bytes for " << 2 * ROUNDS
                        << " finished calls");
        // A call that ended keeps nothing but one timer entry and its token:
        // the call and its group were freed here, not at their deadline.
        CHECK(kept <= 2 * ROUNDS * 450);
    }

    TEST_CASE("a protocol version mismatch fails the handshake") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        comm::detail::Hooks::set_protocol_version(communicator, 1, 99);
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto outcome = co_await group.barrier();
                co_return outcome;
            });
        REQUIRE(errors[0].has_value());
        REQUIRE(errors[1].has_value());
        CHECK(errors[0]->is(dstd::Condition::Unsupported));
        CHECK(dftu_comm_test::has_text(*errors[0], "speaks protocol 1"));
        CHECK(dftu_comm_test::has_text(*errors[0], "rank 1 speaks 99"));
        // One error, one group prefix, and no rank blamed for an abort.
        CHECK(dftu_comm_test::count_text(*errors[0], "group ") == 1);
        CHECK_FALSE(dftu_comm_test::has_text(*errors[0], "aborted by"));
        CHECK(errors[0]->message() == errors[1]->message());
    }

    TEST_CASE("many groups progress at once") {
        Runtime rt(4);
        constexpr std::uint32_t GROUPS = 8;
        dstd::Vector<comm::Communicator> communicators;
        for (std::uint32_t at = 0; at < GROUPS; ++at)
            communicators.push_back(make_communicator(2, *rt.executor()));

        auto errors = dftu_comm_test::run_ranks(
            rt, 2 * GROUPS,
            [&communicators](
                std::uint32_t slot) -> coro::CoroTask<dstd::Result<void>> {
                const std::uint32_t group = slot / 2;
                const std::uint32_t rank = slot % 2;
                comm::Group handle = communicators[group].world(rank);
                auto outcome = co_await handle.barrier();
                co_return outcome;
            });
        for (std::uint32_t slot = 0; slot < 2 * GROUPS; ++slot)
            CHECK_FALSE(errors[slot].has_value());
    }
}
