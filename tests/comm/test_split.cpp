// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/support.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <utility>

using namespace dftracer::utils;

namespace {

comm::Communicator make_communicator(std::uint32_t ranks,
                                     coro::Executor& exec) {
    auto started = comm::Communicator::start_local(ranks, exec);
    REQUIRE(started.has_value());
    return std::move(*started);
}

/// What one rank got out of a split.
struct Child {
    std::uint32_t rank = 0;
    std::uint32_t size = 0;
    std::uint64_t id = 0;
};

}  // namespace

TEST_SUITE("CommSplit") {
    TEST_CASE("split orders the new ranks by key and leaves out no color") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(4, *rt.executor());
        dstd::Vector<Child> children(4);
        auto errors = dftu_comm_test::run_ranks(
            rt, 4,
            [&communicator, &children](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                // Keys run against the parent order, and rank 3 stays out.
                const std::uint32_t key = 3 - rank;
                const std::uint32_t color =
                    rank == 3 ? comm::Group::NO_COLOR : 7;
                auto child = co_await group.split(color, key);
                if (!child) co_return std::move(child).error();
                children[rank] =
                    Child{child->rank(), child->size(), child->id()};
                auto barrier = co_await child->barrier();
                co_return barrier;
            });
        for (std::uint32_t rank = 0; rank < 4; ++rank)
            CHECK_FALSE(errors[rank].has_value());

        CHECK(children[0].size == 3);
        CHECK(children[2].size == 3);
        CHECK(children[0].rank == 2);
        CHECK(children[1].rank == 1);
        CHECK(children[2].rank == 0);
        CHECK(children[0].id == children[2].id);
        CHECK(children[0].id != communicator.world(0).id());

        CHECK(children[3].size == 0);
        CHECK(children[3].rank == 0);
        CHECK(children[3].id == 0);
    }

    TEST_CASE("split breaks equal keys by the rank in the parent group") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(4, *rt.executor());
        dstd::Vector<std::uint32_t> new_ranks(4);
        auto errors = dftu_comm_test::run_ranks(
            rt, 4,
            [&communicator, &new_ranks](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto child = co_await group.split(5, 0);
                if (!child) co_return std::move(child).error();
                new_ranks[rank] = child->rank();
                if (child->size() != 4)
                    co_return dstd::Error{dstd::Condition::Internal,
                                          "the child group lost a rank"};
                co_return {};
            });
        for (std::uint32_t rank = 0; rank < 4; ++rank) {
            CHECK_FALSE(errors[rank].has_value());
            CHECK(new_ranks[rank] == rank);
        }
    }

    TEST_CASE("every rank that passes no color gets an empty group") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto child = co_await group.split(comm::Group::NO_COLOR, 0);
                if (!child) co_return std::move(child).error();
                if (child->size() != 0 || child->id() != 0 ||
                    child->rank() != 0)
                    co_return dstd::Error{dstd::Condition::Internal,
                                          "the empty group is not empty"};
                // The collectives of an empty group end at once.
                auto first = co_await child->barrier();
                if (!first) co_return std::move(first).error();
                auto second = co_await child->split(1, 0);
                if (!second) co_return std::move(second).error();
                if (second->size() != 0)
                    co_return dstd::Error{dstd::Condition::Internal,
                                          "a split of nothing made a group"};
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("two groups of one split progress at once") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(4, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 4,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto child = co_await group.split(rank / 2, rank);
                if (!child) co_return std::move(child).error();
                if (child->size() != 2)
                    co_return dstd::Error{dstd::Condition::Internal,
                                          "a color group lost a rank"};
                // Both children are in flight at the same time, and the
                // parent keeps working after the split.
                auto barrier = co_await child->barrier();
                if (!barrier) co_return std::move(barrier).error();
                auto parent = co_await group.barrier();
                co_return parent;
            });
        for (std::uint32_t rank = 0; rank < 4; ++rank)
            CHECK_FALSE(errors[rank].has_value());
    }

    TEST_CASE("a child group splits again with its own id and sequence") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(4, *rt.executor());
        dstd::Vector<Child> grand(4);
        auto errors = dftu_comm_test::run_ranks(
            rt, 4,
            [&communicator,
             &grand](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto child = co_await group.split(rank / 2, rank);
                if (!child) co_return std::move(child).error();
                auto nested = co_await child->split(rank % 2, 0);
                if (!nested) co_return std::move(nested).error();
                grand[rank] =
                    Child{nested->rank(), nested->size(), nested->id()};
                if (nested->id() == child->id() || nested->id() == group.id())
                    co_return dstd::Error{dstd::Condition::Internal,
                                          "a group reused an id"};
                auto barrier = co_await nested->barrier();
                co_return barrier;
            });
        for (std::uint32_t rank = 0; rank < 4; ++rank) {
            CHECK_FALSE(errors[rank].has_value());
            CHECK(grand[rank].size == 1);
            CHECK(grand[rank].rank == 0);
            CHECK(grand[rank].id != 0);
        }
        CHECK(grand[0].id != grand[1].id);
        CHECK(grand[2].id != grand[3].id);
    }

    TEST_CASE("a rank that splits while a peer barriers is an error") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 0) {
                    auto child = co_await group.split(0, 0);
                    if (!child) co_return std::move(child).error();
                    co_return {};
                }
                auto outcome = co_await group.monitored_barrier();
                co_return outcome;
            });
        REQUIRE(errors[0].has_value());
        REQUIRE(errors[1].has_value());
        CHECK(errors[0]->message() == errors[1]->message());
        CHECK(dftu_comm_test::has_text(*errors[0], "split"));
        CHECK(dftu_comm_test::has_text(*errors[0], "monitored_barrier"));
    }
    TEST_CASE("an error names the ranks of the group that made the call") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(4, *rt.executor());
        std::atomic<bool> finished{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 4,
            [&communicator, &finished](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group parent = communicator.world(rank);
                // Keys run the other way, so world rank 0 is child rank 3.
                auto child = co_await parent.split(0, 3 - rank);
                if (!child) co_return std::move(child).error();
                if (rank != 0) {
                    co_await dftu_comm_test::wait_flag(finished);
                    // The peers never arrive, and the child is aborted.
                    auto late = co_await child->barrier();
                    co_return late;
                }
                comm::CallOpts opts;
                opts.deadline_ns = dftu_comm_test::deadline_after(40'000'000);
                auto late = co_await child->monitored_barrier(opts);
                finished.store(true, std::memory_order_release);
                co_return late;
            });
        REQUIRE(errors[0].has_value());
        CHECK(errors[0]->is(dstd::Condition::Timeout));
        CHECK(dftu_comm_test::has_text(*errors[0], "aborted by rank 3"));
        CHECK(dftu_comm_test::has_text(*errors[0], "missing ranks: 0, 1, 2"));
        CHECK(dftu_comm_test::count_text(*errors[0], "group ") == 1);
        for (std::uint32_t rank = 1; rank < 4; ++rank) {
            REQUIRE(errors[rank].has_value());
            CHECK(errors[rank]->message() == errors[0]->message());
        }
    }
}
