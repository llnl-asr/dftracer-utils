// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/support.h>
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/channel.h>
#include <dftracer/utils/comm/error.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/stop_token.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/hash/fnv1a.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

using namespace dftracer::utils;

namespace {

comm::Communicator make_communicator(std::uint32_t ranks,
                                     coro::Executor& exec) {
    auto started = comm::Communicator::start_local(ranks, exec);
    REQUIRE(started.has_value());
    return std::move(*started);
}

/// A channel of values of one type, named with its type and its name.
struct Tokens {
    using value_type = std::uint32_t;
    static constexpr dstd::StringView name = "tokens";
};

/// A plain value of a fixed size, which a channel carries like an integer.
struct Pair {
    std::uint32_t first = 0;
    std::uint32_t second = 0;
};

/// A channel of bytes, named once for both ends of a test.
using Bytes = dstd::Vector<std::byte>;

comm::ChannelOpts depth_of(std::size_t depth, comm::Overflow overflow) {
    comm::ChannelOpts opts;
    opts.depth = depth;
    opts.overflow = overflow;
    return opts;
}

/// The options of a call that ends @p ns nanoseconds from now.
comm::CallOpts deadline_in(std::uint64_t ns) {
    comm::CallOpts opts;
    opts.deadline_ns = dftu_comm_test::deadline_after(ns);
    return opts;
}

/// The bytes of one message of the test.
Bytes message_of(std::size_t size) {
    Bytes bytes(size);
    for (std::size_t at = 0; at < size; ++at)
        bytes[at] = static_cast<std::byte>(at + 1);
    return bytes;
}

}  // namespace

TEST_SUITE("CommChannel") {
    TEST_CASE("a channel moves a value from one rank to another") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                comm::Channel<std::uint64_t> channel =
                    group.channel<std::uint64_t>("tokens");
                CHECK(channel.name() == "tokens");
                CHECK(channel.id() ==
                      dstd::hash::fnv1a_hash(dstd::StringView{"tokens"}));
                CHECK(channel.size() == 2);
                CHECK(channel.rank() == rank);
                if (rank == 0) {
                    auto sent_op = channel.send(1, 42);
                    auto sent = co_await sent_op;
                    if (!sent) co_return sent;
                    channel.close();
                    co_return {};
                }
                auto message_op = channel.recv_from(0);
                auto message = co_await message_op;
                if (!message) co_return std::move(message).error();
                CHECK(message->from() == 0);
                CHECK(message->value() == 42);
                auto again_op = channel.recv_from(0);
                auto again = co_await again_op;
                CHECK_FALSE(again.has_value());
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a channel of a tag type names its channel and its value") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                comm::Channel<Tokens::value_type> channel =
                    group.channel<Tokens>();
                CHECK(channel.name() == Tokens::name);
                if (rank == 0) {
                    auto returned_op = channel.send(1, 7u);
                    co_return co_await returned_op;
                }
                auto message_op = channel.recv();
                auto message = co_await message_op;
                if (!message) co_return std::move(message).error();
                CHECK(message->from() == 0);
                CHECK(message->value() == 7u);
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a channel moves a plain struct of a fixed size") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<Pair>("pair");
                if (rank == 0) {
                    auto returned_op = channel.send(1, Pair{7, 9});
                    co_return co_await returned_op;
                }
                auto message_op = channel.recv_from(0);
                auto message = co_await message_op;
                if (!message) co_return std::move(message).error();
                CHECK(message->value().first == 7);
                CHECK(message->value().second == 9);
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a receive of any source gives the rank that sent it") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(3, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("any");
                if (rank != 0) {
                    auto returned_op = channel.send(0, rank);
                    co_return co_await returned_op;
                }
                for (std::uint32_t seen = 0; seen < 2; ++seen) {
                    auto message_op = channel.recv();
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    CHECK(message->value() == message->from());
                }
                co_return {};
            });
        for (std::uint32_t rank = 0; rank < 3; ++rank)
            CHECK_FALSE(errors[rank].has_value());
    }

    TEST_CASE("a byte channel receives into a caller buffer") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        const Bytes sent = message_of(5);
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator,
             &sent](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<Bytes>("bytes");
                if (rank == 0) {
                    auto returned_op = channel.send(
                        1,
                        dstd::Span<const std::byte>(sent.data(), sent.size()));
                    co_return co_await returned_op;
                }
                Bytes buffer(8);
                auto taken_op = channel.recv_into(
                    dstd::Span<std::byte>(buffer.data(), buffer.size()));
                auto taken = co_await taken_op;
                if (!taken) co_return std::move(taken).error();
                CHECK(*taken == sent.size());
                CHECK(buffer[0] == sent[0]);
                CHECK(buffer[sent.size() - 1] == sent[sent.size() - 1]);
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a short buffer fails with TooBig and keeps the message") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        const Bytes sent = message_of(5);
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator,
             &sent](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<Bytes>("too-big");
                if (rank == 0) {
                    auto returned_op = channel.send(
                        1,
                        dstd::Span<const std::byte>(sent.data(), sent.size()));
                    co_return co_await returned_op;
                }
                Bytes small(2);
                auto refused_op = channel.recv_into(
                    dstd::Span<std::byte>(small.data(), small.size()));
                auto refused = co_await refused_op;
                REQUIRE_FALSE(refused.has_value());
                CHECK(refused.error().is_code(comm::ChannelError::TooBig));
                CHECK(dftu_comm_test::has_text(refused.error(), "too-big"));
                CHECK(dftu_comm_test::has_text(refused.error(), "rank 0"));

                // The message the short buffer refused is still first.
                Bytes big(8);
                auto taken_op = channel.recv_into(
                    dstd::Span<std::byte>(big.data(), big.size()));
                auto taken = co_await taken_op;
                REQUIRE(taken.has_value());
                CHECK(*taken == sent.size());
                CHECK(big[0] == sent[0]);
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("the messages of one sender arrive in the order they were sent") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        constexpr std::uint32_t COUNT = 64;
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>(
                    "fifo", depth_of(4, comm::Overflow::Block));
                if (rank == 0) {
                    for (std::uint32_t value = 0; value < COUNT; ++value) {
                        auto sent_op = channel.send(1, value);
                        auto sent = co_await sent_op;
                        if (!sent) co_return sent;
                    }
                    co_return {};
                }
                for (std::uint32_t expected = 0; expected < COUNT; ++expected) {
                    auto message_op = channel.recv_from(0);
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    if (message->value() != expected)
                        co_return dstd::Error{
                            dstd::Condition::Internal,
                            "the message order of one sender differs"};
                }
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("the messages of many senders keep the order of each") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(3, *rt.executor());
        constexpr std::uint32_t COUNT = 32;
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>(
                    "many", depth_of(2, comm::Overflow::Block));
                if (rank != 0) {
                    for (std::uint32_t value = 0; value < COUNT; ++value) {
                        auto sent_op = channel.send(0, value);
                        auto sent = co_await sent_op;
                        if (!sent) co_return sent;
                    }
                    co_return {};
                }
                for (std::uint32_t round = 0; round < COUNT; ++round) {
                    for (std::uint32_t peer = 1; peer < 3; ++peer) {
                        auto message_op = channel.recv_from(peer);
                        auto message = co_await message_op;
                        if (!message) co_return std::move(message).error();
                        if (message->from() != peer ||
                            message->value() != round)
                            co_return dstd::Error{
                                dstd::Condition::Internal,
                                "the message order of a sender differs"};
                    }
                }
                co_return {};
            });
        for (std::uint32_t rank = 0; rank < 3; ++rank)
            CHECK_FALSE(errors[rank].has_value());
    }

    TEST_CASE(
        "recv_in_rank_order gives one message from each rank in rank order") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(4, *rt.executor());
        constexpr std::uint32_t COUNT = 3;
        auto errors = dftu_comm_test::run_ranks(
            rt, 4,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("rank-order");
                if (rank != 0) {
                    for (std::uint32_t value = 0; value < COUNT; ++value) {
                        auto sent_op = channel.send(0, rank);
                        auto sent = co_await sent_op;
                        if (!sent) co_return sent;
                    }
                    co_return {};
                }
                for (std::uint32_t round = 0; round < COUNT; ++round) {
                    for (std::uint32_t order = 1; order < 4; ++order) {
                        auto message_op = channel.recv_in_rank_order();
                        auto message = co_await message_op;
                        if (!message) co_return std::move(message).error();
                        if (message->from() != order)
                            co_return dstd::Error{
                                dstd::Condition::Internal,
                                "the rank order of the receives differs"};
                    }
                }
                co_return {};
            });
        for (std::uint32_t rank = 0; rank < 4; ++rank)
            CHECK_FALSE(errors[rank].has_value());
    }

    TEST_CASE("a Full stream fails a send at once and leaves the group") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> refused{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &refused](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>(
                    "full", depth_of(1, comm::Overflow::Fail));
                if (rank == 0) {
                    auto first_op = channel.send(1, 1u);
                    auto first = co_await first_op;
                    if (!first) co_return first;
                    auto second_op = channel.send(1, 2u);
                    auto second = co_await second_op;
                    REQUIRE_FALSE(second.has_value());
                    CHECK(second.error().is_code(comm::ChannelError::Full));
                    CHECK(dftu_comm_test::has_text(second.error(), "full"));
                    CHECK(dftu_comm_test::has_text(second.error(), "rank 1"));
                    refused.store(true, std::memory_order_release);
                    // The group of the two ranks still barrierks together.
                    auto returned_op = group.barrier();
                    co_return co_await returned_op;
                }
                co_await dftu_comm_test::wait_flag(refused);
                auto message_op = channel.recv_from(0);
                auto message = co_await message_op;
                if (!message) co_return std::move(message).error();
                CHECK(message->value() == 1u);
                auto returned_op = group.barrier();
                co_return co_await returned_op;
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("two values of one sender both reach their receive") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>(
                    "two", depth_of(1, comm::Overflow::Block));
                if (rank == 0) {
                    auto first_op = channel.send(1, 10u);
                    auto first = co_await first_op;
                    if (!first) co_return first;
                    auto returned_op = channel.send(1, 11u);
                    co_return co_await returned_op;
                }
                for (std::uint32_t expected = 10; expected <= 11; ++expected) {
                    auto message_op = channel.recv_from(0);
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    if (message->value() != expected)
                        co_return dstd::Error{dstd::Condition::Internal,
                                              "the value differs"};
                }
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a Block stream makes a send wait for a receive") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> posted{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &posted](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>(
                    "block", depth_of(1, comm::Overflow::Block));
                if (rank == 0) {
                    auto first_op = channel.send(1, 1u);
                    auto first = co_await first_op;
                    if (!first) co_return first;
                    // The stream holds one message, so this send waits.
                    comm::Op<void> second = channel.send(1, 2u);
                    CHECK_FALSE(second.test());
                    posted.store(true, std::memory_order_release);
                    auto waited = co_await second;
                    if (!waited) co_return waited;
                    auto returned_op = channel.send(1, 3u);
                    co_return co_await returned_op;
                }
                co_await dftu_comm_test::wait_flag(posted);
                for (std::uint32_t expected = 1; expected <= 3; ++expected) {
                    auto message_op = channel.recv_from(0);
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    if (message->value() != expected)
                        co_return dstd::Error{
                            dstd::Condition::Internal,
                            "the message a Block stream held came late"};
                }
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a DropOldest stream keeps the newest message") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> sent_all{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &sent_all](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>(
                    "newest", depth_of(1, comm::Overflow::DropOldest));
                if (rank == 0) {
                    for (std::uint32_t value = 1; value <= 3; ++value) {
                        auto sent_op = channel.send(1, value);
                        auto sent = co_await sent_op;
                        if (!sent) co_return sent;
                    }
                    sent_all.store(true, std::memory_order_release);
                    co_return {};
                }
                co_await dftu_comm_test::wait_flag(sent_all);
                auto message_op = channel.recv_from(0);
                auto message = co_await message_op;
                if (!message) co_return std::move(message).error();
                CHECK(message->value() == 3u);
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a close drains every message and then fails the receive") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("closing");
                if (rank == 0) {
                    for (std::uint32_t value = 0; value < 3; ++value) {
                        auto sent_op = channel.send(1, value);
                        auto sent = co_await sent_op;
                        if (!sent) co_return sent;
                    }
                    channel.close();
                    auto late_op = channel.send(1, 3u);
                    auto late = co_await late_op;
                    REQUIRE_FALSE(late.has_value());
                    CHECK(late.error().is_code(comm::ChannelError::Closed));
                    CHECK(dftu_comm_test::has_text(late.error(), "closing"));
                    co_return {};
                }
                for (std::uint32_t expected = 0; expected < 3; ++expected) {
                    auto message_op = channel.recv_from(0);
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    if (message->value() != expected)
                        co_return dstd::Error{
                            dstd::Condition::Internal,
                            "the messages before a close differ"};
                }
                auto closed_op = channel.recv_from(0);
                auto closed = co_await closed_op;
                REQUIRE_FALSE(closed.has_value());
                CHECK(closed.error().is_code(comm::ChannelError::Closed));
                CHECK(dftu_comm_test::has_text(closed.error(), "closing"));
                CHECK(dftu_comm_test::has_text(closed.error(), "rank 0"));
                // A close stands, so the next receive says so at once.
                auto again_op = channel.recv();
                auto again = co_await again_op;
                REQUIRE_FALSE(again.has_value());
                CHECK(again.error().is_code(comm::ChannelError::Closed));
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("cancelling a receive keeps its message") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> cancelled{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &cancelled](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>(
                    "cancel-recv", depth_of(1, comm::Overflow::Block));
                if (rank == 0) {
                    comm::Op<comm::Message<std::uint32_t>> waiting =
                        channel.recv_from(1);
                    CHECK_FALSE(waiting.test());
                    waiting.cancel();
                    CHECK(waiting.status() == comm::OpStatus::Canceled);
                    cancelled.store(true, std::memory_order_release);
                    // The message the cancelled receive never took arrives.
                    auto message_op = channel.recv_from(1);
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    CHECK(message->from() == 1);
                    CHECK(message->value() == 9u);
                    co_return {};
                }
                co_await dftu_comm_test::wait_flag(cancelled);
                auto returned_op = channel.send(0, 9u);
                co_return co_await returned_op;
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("cancelling a send ends that send alone") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> cancelled{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &cancelled](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>(
                    "cancel-send", depth_of(1, comm::Overflow::Block));
                if (rank == 0) {
                    auto first_op = channel.send(1, 1u);
                    auto first = co_await first_op;
                    if (!first) co_return first;
                    comm::Op<void> waiting = channel.send(1, 2u);
                    CHECK_FALSE(waiting.test());
                    waiting.cancel();
                    CHECK(waiting.status() == comm::OpStatus::Canceled);
                    cancelled.store(true, std::memory_order_release);
                    // The group of the call stands, so the collective works.
                    auto returned_op = group.barrier();
                    co_return co_await returned_op;
                }
                co_await dftu_comm_test::wait_flag(cancelled);
                auto message_op = channel.recv_from(0);
                auto message = co_await message_op;
                if (!message) co_return std::move(message).error();
                CHECK(message->value() == 1u);
                auto together_op = group.barrier();
                auto together = co_await together_op;
                if (!together) co_return together;
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("an abort fails every pending send and receive with one error") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<int> parked{0};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &parked](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto receiving = group.channel<std::uint32_t>("abort-recv");
                auto sending = group.channel<std::uint32_t>(
                    "abort-send", depth_of(1, comm::Overflow::Block));
                if (rank == 0) {
                    comm::Op<comm::Message<std::uint32_t>> waiting =
                        receiving.recv_from(1);
                    parked.fetch_add(1, std::memory_order_release);
                    auto ended = co_await waiting;
                    REQUIRE_FALSE(ended.has_value());
                    CHECK(dftu_comm_test::has_text(ended.error(), "gives up"));
                    CHECK(ended.error().is(dstd::Condition::Internal));
                    parked.fetch_add(1, std::memory_order_release);
                    co_return {};
                }
                auto first_op = sending.send(0, 1u);
                auto first = co_await first_op;
                if (!first) co_return first;
                comm::Op<void> waiting = sending.send(0, 2u);
                CHECK_FALSE(waiting.test());
                parked.fetch_add(1, std::memory_order_release);
                co_await dftu_comm_test::wait_count(parked, 2);
                group.abort(
                    dstd::Error{dstd::Condition::Internal, "rank 1 gives up"});
                auto ended = co_await waiting;
                REQUIRE_FALSE(ended.has_value());
                CHECK(dftu_comm_test::has_text(ended.error(), "gives up"));
                // A call made after the abort fails at once, with the same
                // error.
                auto later_op = sending.send(0, 3u);
                auto later = co_await later_op;
                REQUIRE_FALSE(later.has_value());
                CHECK(later.error().message() == ended.error().message());
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a channel and a collective interleave without interference") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        constexpr std::uint32_t ROUNDS = 4;
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto left = group.channel<std::uint32_t>("left");
                auto right = group.channel<std::uint32_t>("right");
                for (std::uint32_t round = 0; round < ROUNDS; ++round) {
                    if (rank == 0) {
                        auto first_op = left.send(1, round);
                        auto first = co_await first_op;
                        if (!first) co_return first;
                        auto second_op = right.send(1, round + 100);
                        auto second = co_await second_op;
                        if (!second) co_return second;
                    } else {
                        auto first_op = left.recv_from(0);
                        auto first = co_await first_op;
                        if (!first) co_return std::move(first).error();
                        auto second_op = right.recv_from(0);
                        auto second = co_await second_op;
                        if (!second) co_return std::move(second).error();
                        if (first->value() != round ||
                            second->value() != round + 100)
                            co_return dstd::Error{
                                dstd::Condition::Internal,
                                "a channel took the message of another"};
                    }
                    auto together_op = group.barrier();
                    auto together = co_await together_op;
                    if (!together) co_return together;
                }
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a value type mismatch names the channel and both ranks") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 0) {
                    auto wide = group.channel<std::uint64_t>("same");
                    comm::Op<void> sent = wide.send(1, 1ull);
                    CHECK(sent.test());
                    auto returned_op = group.barrier();
                    co_return co_await returned_op;
                }
                auto narrow = group.channel<std::uint32_t>("same");
                auto message_op = narrow.recv_from(0);
                auto message = co_await message_op;
                REQUIRE_FALSE(message.has_value());
                CHECK(
                    message.error().is_code(comm::ChannelError::WrongChannel));
                CHECK(dftu_comm_test::has_text(message.error(), "same"));
                CHECK(dftu_comm_test::has_text(message.error(), "rank 0"));
                CHECK(dftu_comm_test::has_text(message.error(), "rank 1"));
                CHECK(dftu_comm_test::has_text(message.error(), "4 bytes"));
                CHECK(dftu_comm_test::has_text(message.error(), "8 bytes"));
                auto returned_op = group.barrier();
                co_return co_await returned_op;
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE(
        "a name mismatch ends by its deadline and names what was declared") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 0) {
                    auto elsewhere = group.channel<std::uint32_t>("elsewhere");
                    comm::Op<void> sent = elsewhere.send(1, 1u);
                    CHECK(sent.test());
                    auto returned_op = group.barrier();
                    co_return co_await returned_op;
                }
                auto awaited = group.channel<std::uint32_t>("awaited");
                auto message_op =
                    awaited.recv_from(0, deadline_in(100'000'000ull));
                auto message = co_await message_op;
                REQUIRE_FALSE(message.has_value());
                CHECK(message.error().is(dstd::Condition::Timeout));
                CHECK(
                    dftu_comm_test::has_text(message.error(), "did not open"));
                CHECK(dftu_comm_test::has_text(message.error(), "rank 0"));
                auto returned_op = group.barrier();
                co_return co_await returned_op;
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("split gives two ends that move in both directions") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("both");
                comm::ChannelHalves<std::uint32_t> halves = channel.split();
                // A half is a handle, so a copy names the same channel.
                comm::ChannelTx<std::uint32_t> tx = halves.tx;
                if (rank == 0) {
                    auto up_op = tx.send(1, 10u);
                    auto up = co_await up_op;
                    if (!up) co_return up;
                    auto back_op = halves.rx.recv_from(1);
                    auto back = co_await back_op;
                    if (!back) co_return std::move(back).error();
                    CHECK(back->value() == 20u);
                    co_return {};
                }
                auto down_op = halves.rx.recv_from(0);
                auto down = co_await down_op;
                if (!down) co_return std::move(down).error();
                CHECK(down->value() == 10u);
                auto returned_op = tx.send(0, 20u);
                co_return co_await returned_op;
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a channel of a group of one rank fails its calls at once") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(1, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 1,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("alone");
                CHECK(channel.id() == 0);
                auto sent_op = channel.send(0, 1u);
                auto sent = co_await sent_op;
                REQUIRE_FALSE(sent.has_value());
                CHECK(sent.error().is(dstd::Condition::InvalidArgument));
                CHECK(dftu_comm_test::has_text(sent.error(), "two ranks"));
                auto message_op = channel.recv();
                auto message = co_await message_op;
                REQUIRE_FALSE(message.has_value());
                CHECK(message.error().is(dstd::Condition::InvalidArgument));
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
    }

    TEST_CASE("a channel tag never meets a collective tag") {
        using comm::backend::message_tag;
        using comm::backend::MessageKey;
        using comm::backend::tag_index;
        using comm::backend::tag_purpose;
        using comm::backend::TagPurpose;

        // Every purpose takes a quarter of the tag space, so no index of one
        // reaches another.
        CHECK(tag_purpose(message_tag(TagPurpose::ChannelMessage, 0)) ==
              TagPurpose::ChannelMessage);
        CHECK(tag_purpose(message_tag(TagPurpose::CollectiveTable, 0)) ==
              TagPurpose::CollectiveTable);
        for (const std::uint64_t index :
             {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{1} << 40}) {
            CHECK(message_tag(TagPurpose::ChannelMessage, index) !=
                  message_tag(TagPurpose::CollectiveRecord, index));
            CHECK(message_tag(TagPurpose::ChannelMessage, index) !=
                  message_tag(TagPurpose::CollectiveTable, index));
            CHECK(message_tag(TagPurpose::ChannelOpen, index) !=
                  message_tag(TagPurpose::ChannelMessage, index));
            CHECK(message_tag(TagPurpose::ChannelMessage, index) !=
                  message_tag(TagPurpose::ChannelMessage, index + 1));
        }
        // An index wider than the space of its purpose keeps its purpose.
        const std::uint64_t wide =
            message_tag(TagPurpose::ChannelMessage, ~std::uint64_t{0});
        CHECK(tag_purpose(wide) == TagPurpose::ChannelMessage);
        CHECK(message_tag(TagPurpose::ChannelMessage, tag_index(wide)) == wide);
    }

    TEST_CASE("a channel call reports its cost per message") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        constexpr std::uint32_t ROUNDS = 2000;
        std::atomic<std::uint64_t> ping_pong_ns{0};
        std::atomic<std::uint64_t> stream_ns{0};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &ping_pong_ns, &stream_ns](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint64_t>(
                    "cost", depth_of(64, comm::Overflow::Block));
                if (rank == 0) {
                    const std::uint64_t started = dstd::steady_now_ns();
                    for (std::uint32_t round = 0; round < ROUNDS; ++round) {
                        auto sent_op = channel.send(1, round);
                        auto sent = co_await sent_op;
                        if (!sent) co_return sent;
                        auto back_op = channel.recv_from(1);
                        auto back = co_await back_op;
                        if (!back) co_return std::move(back).error();
                    }
                    ping_pong_ns.store(dstd::steady_now_ns() - started,
                                       std::memory_order_release);
                    const std::uint64_t streaming = dstd::steady_now_ns();
                    for (std::uint32_t round = 0; round < ROUNDS; ++round) {
                        auto sent_op = channel.send(1, round);
                        auto sent = co_await sent_op;
                        if (!sent) co_return sent;
                    }
                    stream_ns.store(dstd::steady_now_ns() - streaming,
                                    std::memory_order_release);
                    co_return {};
                }
                for (std::uint32_t round = 0; round < ROUNDS; ++round) {
                    auto message_op = channel.recv_from(0);
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    auto sent_op = channel.send(0, message->value());
                    auto sent = co_await sent_op;
                    if (!sent) co_return sent;
                }
                for (std::uint32_t round = 0; round < ROUNDS; ++round) {
                    auto message_op = channel.recv_from(0);
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                }
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
        const double ping = static_cast<double>(ping_pong_ns.load()) /
                            static_cast<double>(2 * ROUNDS);
        const double stream =
            static_cast<double>(stream_ns.load()) / static_cast<double>(ROUNDS);
        std::printf(
            "[channel cost] ping-pong %.0f ns per message, streaming %.0f ns "
            "per message\n",
            ping, stream);
        CHECK(ping > 0.0);
        CHECK(stream > 0.0);
    }
    TEST_CASE("a cancel after a completed send takes nothing back") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> cancelled{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &cancelled](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("late-cancel");
                if (rank == 0) {
                    comm::Op<void> sent = channel.send(1, 7u);
                    auto done = co_await sent;
                    if (!done) co_return done;
                    sent.cancel();
                    CHECK(sent.status() == comm::OpStatus::Ok);
                    cancelled.store(true, std::memory_order_release);
                    co_return {};
                }
                co_await dftu_comm_test::wait_flag(cancelled);
                auto message_op =
                    channel.recv_from(0, deadline_in(5'000'000'000ull));
                auto message = co_await message_op;
                if (!message) co_return std::move(message).error();
                CHECK(message->value() == 7u);
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("receives cancelled at any moment lose no message") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        constexpr std::uint32_t COUNT = 3000;
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>(
                    "cancel-race", depth_of(4, comm::Overflow::Block));
                if (rank == 1) {
                    for (std::uint32_t value = 0; value < COUNT; ++value) {
                        auto sent_op = channel.send(
                            0, value, deadline_in(20'000'000'000ull));
                        auto sent = co_await sent_op;
                        if (!sent) co_return sent;
                    }
                    co_return {};
                }
                std::uint32_t next = 0;
                while (next < COUNT) {
                    auto call =
                        channel.recv_from(1, deadline_in(20'000'000'000ull));
                    for (std::uint32_t turn = 0; turn < next % 7; ++turn)
                        co_await coro::yield();
                    call.cancel();
                    // A cancel that lost the race to a message leaves the call
                    // its message, which arrives through the provider.
                    auto outcome = call.wait();
                    if (!outcome) {
                        if (outcome.error().is(dstd::Condition::Cancelled))
                            continue;
                        co_return std::move(outcome).error();
                    }
                    if (outcome->value() != next)
                        co_return dstd::Error{
                            dstd::Condition::Internal,
                            "a cancelled receive lost or reordered a message"};
                    ++next;
                }
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a call with a stopped token never reaches its stream") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> receive_cancelled{false};
        std::atomic<bool> send_cancelled{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &receive_cancelled, &send_cancelled](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("stopped");
                coro::StopSource source;
                source.request_stop();
                comm::CallOpts stopped;
                stopped.stop = source.token();
                if (rank == 0) {
                    comm::Op<comm::Message<std::uint32_t>> waiting =
                        channel.recv_from(1, stopped);
                    CHECK(waiting.status() == comm::OpStatus::Canceled);
                    receive_cancelled.store(true, std::memory_order_release);
                    // The receive that was stopped took nothing, so the
                    // message of the sender reaches this one.
                    auto message_op =
                        channel.recv_from(1, deadline_in(5'000'000'000ull));
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    CHECK(message->value() == 9u);
                    co_await dftu_comm_test::wait_flag(send_cancelled);
                    auto never_op =
                        channel.recv_from(1, deadline_in(100'000'000ull));
                    auto never = co_await never_op;
                    REQUIRE_FALSE(never.has_value());
                    CHECK(never.error().is(dstd::Condition::Timeout));
                    co_return {};
                }
                co_await dftu_comm_test::wait_flag(receive_cancelled);
                auto sent_op = channel.send(0, 9u);
                auto sent = co_await sent_op;
                if (!sent) co_return sent;
                comm::Op<void> refused = channel.send(0, 10u, stopped);
                CHECK(refused.status() == comm::OpStatus::Canceled);
                send_cancelled.store(true, std::memory_order_release);
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a cancel while a receive waits for the open of its peer") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> cancelled{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &cancelled](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("open-cancel");
                if (rank == 0) {
                    comm::Op<comm::Message<std::uint32_t>> waiting =
                        channel.recv_from(1);
                    CHECK_FALSE(waiting.test());
                    waiting.cancel();
                    CHECK(waiting.status() == comm::OpStatus::Canceled);
                    cancelled.store(true, std::memory_order_release);
                    auto message_op =
                        channel.recv_from(1, deadline_in(5'000'000'000ull));
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    CHECK(message->value() == 3u);
                    co_return {};
                }
                co_await dftu_comm_test::wait_flag(cancelled);
                auto returned_op = channel.send(0, 3u);
                co_return co_await returned_op;
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a close without a send still ends the receive with Closed") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("bare-close");
                if (rank == 0) {
                    channel.close();
                    co_return {};
                }
                auto closed_op =
                    channel.recv_from(0, deadline_in(5'000'000'000ull));
                auto closed = co_await closed_op;
                REQUIRE_FALSE(closed.has_value());
                CHECK(closed.error().is_code(comm::ChannelError::Closed));
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a receive of any sender ends Closed after every peer closed") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(3, *rt.executor());
        std::atomic<bool> one_closed{false};
        std::atomic<bool> second_posted{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator, &one_closed, &second_posted](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("any-close");
                if (rank == 1) {
                    auto sent_op = channel.send(0, 1u);
                    auto sent = co_await sent_op;
                    if (!sent) co_return sent;
                    channel.close();
                    one_closed.store(true, std::memory_order_release);
                    co_return {};
                }
                if (rank == 2) {
                    co_await dftu_comm_test::wait_flag(second_posted);
                    auto sent_op = channel.send(0, 2u);
                    auto sent = co_await sent_op;
                    if (!sent) co_return sent;
                    channel.close();
                    co_return {};
                }
                auto first_op = channel.recv(deadline_in(5'000'000'000ull));
                auto first = co_await first_op;
                if (!first) co_return std::move(first).error();
                CHECK(first->from() == 1);
                CHECK(first->value() == 1u);
                co_await dftu_comm_test::wait_flag(one_closed);
                // Rank 1 closed and rank 2 did not, so this receive waits.
                comm::Op<comm::Message<std::uint32_t>> second =
                    channel.recv(deadline_in(5'000'000'000ull));
                CHECK_FALSE(second.test());
                second_posted.store(true, std::memory_order_release);
                auto got = co_await second;
                if (!got) co_return std::move(got).error();
                CHECK(got->from() == 2);
                CHECK(got->value() == 2u);
                auto closed_op = channel.recv(deadline_in(5'000'000'000ull));
                auto closed = co_await closed_op;
                REQUIRE_FALSE(closed.has_value());
                CHECK(closed.error().is_code(comm::ChannelError::Closed));
                CHECK(dftu_comm_test::has_text(closed.error(), "any-close"));
                CHECK_FALSE(dftu_comm_test::has_text(closed.error(), "rank 0"));
                co_return {};
            });
        for (std::uint32_t rank = 0; rank < 3; ++rank)
            CHECK_FALSE(errors[rank].has_value());
    }

    TEST_CASE("two receives of two channels on one peer both get their open") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> posted{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &posted](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto first = group.channel<std::uint32_t>("open-first");
                auto second = group.channel<std::uint32_t>("open-second");
                if (rank == 1) {
                    co_await dftu_comm_test::wait_flag(posted);
                    auto sent_op = second.send(0, 5u);
                    auto sent = co_await sent_op;
                    if (!sent) co_return sent;
                    auto returned_op = first.send(0, 4u);
                    co_return co_await returned_op;
                }
                comm::Op<comm::Message<std::uint32_t>> on_first =
                    first.recv_from(1, deadline_in(5'000'000'000ull));
                comm::Op<comm::Message<std::uint32_t>> on_second =
                    second.recv_from(1, deadline_in(5'000'000'000ull));
                posted.store(true, std::memory_order_release);
                auto got_second = co_await on_second;
                if (!got_second) co_return std::move(got_second).error();
                CHECK(got_second->value() == 5u);
                auto got_first = co_await on_first;
                if (!got_first) co_return std::move(got_first).error();
                CHECK(got_first->value() == 4u);
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a receive of any sender names the channels its peer declared") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 1) {
                    auto elsewhere = group.channel<std::uint32_t>("elsewhere");
                    auto returned_op = elsewhere.send(0, 5u);
                    co_return co_await returned_op;
                }
                auto awaited = group.channel<std::uint32_t>("awaited");
                auto message_op = awaited.recv(deadline_in(100'000'000ull));
                auto message = co_await message_op;
                REQUIRE_FALSE(message.has_value());
                CHECK(message.error().is(dstd::Condition::Timeout));
                CHECK(
                    dftu_comm_test::has_text(message.error(), "did not open"));
                CHECK(dftu_comm_test::has_text(message.error(), "rank 1"));
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a receive into a buffer names the channels its peer declared") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 1) {
                    auto elsewhere = group.channel<Bytes>("elsewhere-bytes");
                    const Bytes sent = message_of(4);
                    auto returned_op = elsewhere.send(
                        0,
                        dstd::Span<const std::byte>(sent.data(), sent.size()));
                    co_return co_await returned_op;
                }
                auto awaited = group.channel<Bytes>("awaited-bytes");
                Bytes buffer(8);
                auto taken_op = awaited.recv_into(
                    dstd::Span<std::byte>(buffer.data(), buffer.size()),
                    deadline_in(100'000'000ull));
                auto taken = co_await taken_op;
                REQUIRE_FALSE(taken.has_value());
                CHECK(taken.error().is(dstd::Condition::Timeout));
                CHECK(dftu_comm_test::has_text(taken.error(), "did not open"));
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a receive of any sender serves every sender in turn") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(3, *rt.executor());
        std::atomic<int> sent_all{0};
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator, &sent_all](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>(
                    "turns", depth_of(8, comm::Overflow::Block));
                if (rank != 0) {
                    for (std::uint32_t at = 0; at < 4; ++at) {
                        auto sent_op = channel.send(0, rank);
                        auto sent = co_await sent_op;
                        if (!sent) co_return sent;
                    }
                    sent_all.fetch_add(1, std::memory_order_release);
                    co_return {};
                }
                co_await dftu_comm_test::wait_count(sent_all, 2);
                for (std::uint32_t at = 0; at < 8; ++at) {
                    auto message_op =
                        channel.recv(deadline_in(5'000'000'000ull));
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    CHECK(message->from() == 1 + at % 2);
                }
                co_return {};
            });
        for (std::uint32_t rank = 0; rank < 3; ++rank)
            CHECK_FALSE(errors[rank].has_value());
    }

    TEST_CASE("a cancelled receive in rank order keeps the turn of its rank") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(3, *rt.executor());
        std::atomic<bool> cancelled{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator, &cancelled](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("turn-order");
                if (rank != 0) {
                    co_await dftu_comm_test::wait_flag(cancelled);
                    auto returned_op = channel.send(0, rank);
                    co_return co_await returned_op;
                }
                comm::Op<comm::Message<std::uint32_t>> skipped =
                    channel.recv_in_rank_order();
                CHECK_FALSE(skipped.test());
                skipped.cancel();
                CHECK(skipped.status() == comm::OpStatus::Canceled);
                cancelled.store(true, std::memory_order_release);
                for (std::uint32_t expected = 1; expected <= 2; ++expected) {
                    auto message_op = channel.recv_in_rank_order(
                        deadline_in(5'000'000'000ull));
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    CHECK(message->from() == expected);
                }
                co_return {};
            });
        for (std::uint32_t rank = 0; rank < 3; ++rank)
            CHECK_FALSE(errors[rank].has_value());
    }
    TEST_CASE("a cancel of the waiting task ends a channel receive alone") {
        dftu_comm_test::QueueExecutor exec;
        comm::Communicator communicator = make_communicator(2, exec);
        comm::Group world0 = communicator.world(0);
        auto channel = world0.channel<std::uint32_t>("task-cancel");

        dstd::Optional<comm::Op<comm::Message<std::uint32_t>>> parked;
        std::atomic<bool> resumed{false};
        dstd::Optional<dstd::Error> woke_with;
        auto body = [&]() -> coro::detail::Coro {
            parked = channel.recv_from(1);
            auto outcome = co_await *parked;
            if (!outcome) woke_with = std::move(outcome).error();
            resumed.store(true, std::memory_order_release);
        };
        auto frame = body();
        exec.post(frame.release());
        exec.run();
        CHECK_FALSE(resumed.load());
        CHECK(exec.parked() == 1);

        exec.cancel_all();
        exec.run();
        CHECK(resumed.load());
        CHECK(exec.parked() == 0);
        REQUIRE(woke_with.has_value());
        CHECK(woke_with->is(dstd::Condition::Cancelled));
        // The group stands: a later send reaches a later receive.
        comm::Group world1 = communicator.world(1);
        auto peer = world1.channel<std::uint32_t>("task-cancel");
        CHECK(peer.send(0, 6u).wait().has_value());
        auto later = channel.recv_from(1).wait();
        REQUIRE(later.has_value());
        CHECK(later->value() == 6u);
    }

    TEST_CASE("a cancelled receive leaves the wait of another channel alone") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> cancelled{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &cancelled](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto first = group.channel<std::uint32_t>("wait-first");
                auto second = group.channel<std::uint32_t>("wait-second");
                if (rank == 0) {
                    auto kept =
                        first.recv_from(1, deadline_in(20'000'000'000ull));
                    auto dropped = second.recv_from(1);
                    dropped.cancel();
                    cancelled.store(true, std::memory_order_release);
                    auto message = co_await kept;
                    if (!message) co_return std::move(message).error();
                    CHECK(message->value() == 1u);
                    co_return {};
                }
                co_await dftu_comm_test::wait_flag(cancelled);
                auto returned_op = first.send(0, 1u);
                co_return co_await returned_op;
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a receive from a rank the group does not have fails at once") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        comm::Group group = communicator.world(0);
        auto channel = group.channel<std::uint32_t>("no-peer");
        comm::Op<comm::Message<std::uint32_t>> missing = channel.recv_from(5);
        CHECK(missing.status() == comm::OpStatus::Failed);
        auto ended = missing.wait();
        REQUIRE_FALSE(ended.has_value());
        CHECK(ended.error().is(dstd::Condition::InvalidArgument));
    }

    TEST_CASE("a close of a channel a peer never opened leaves the others") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(3, *rt.executor());
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 1) {
                    auto unrelated = group.channel<std::uint32_t>("unrelated");
                    auto shared = group.channel<std::uint32_t>("shared");
                    unrelated.close();
                    auto returned_op = shared.send(0, 5u);
                    co_return co_await returned_op;
                }
                if (rank == 0) {
                    auto shared = group.channel<std::uint32_t>("shared");
                    auto message_op =
                        shared.recv_from(1, deadline_in(5'000'000'000ull));
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    CHECK(message->value() == 5u);
                }
                co_return {};
            });
        for (std::uint32_t rank = 0; rank < 3; ++rank)
            CHECK_FALSE(errors[rank].has_value());
    }

    TEST_CASE("a value size mismatch fails every receive of the channel") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> posted{false};
        std::atomic<bool> sent{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &posted,
             &sent](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 1) {
                    auto wide = group.channel<std::uint64_t>("sized");
                    co_await dftu_comm_test::wait_flag(posted);
                    comm::Op<void> call = wide.send(0, 5ull);
                    sent.store(true, std::memory_order_release);
                    co_return {};
                }
                auto narrow = group.channel<std::uint32_t>("sized");
                auto first = narrow.recv_from(1, deadline_in(5'000'000'000ull));
                auto second =
                    narrow.recv_from(1, deadline_in(5'000'000'000ull));
                posted.store(true, std::memory_order_release);
                auto one = co_await first;
                auto two = co_await second;
                REQUIRE_FALSE(one.has_value());
                REQUIRE_FALSE(two.has_value());
                CHECK(one.error().is_code(comm::ChannelError::WrongChannel));
                CHECK(two.error().is_code(comm::ChannelError::WrongChannel));
                co_await dftu_comm_test::wait_flag(sent);
                auto later_op =
                    narrow.recv_from(1, deadline_in(5'000'000'000ull));
                auto later = co_await later_op;
                REQUIRE_FALSE(later.has_value());
                CHECK(later.error().is_code(comm::ChannelError::WrongChannel));
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("a channel a peer declared before this rank opened it matches") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(2, *rt.executor());
        std::atomic<bool> declared{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 2,
            [&communicator, &declared](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 1) {
                    auto early = group.channel<std::uint32_t>("early");
                    auto other = group.channel<std::uint32_t>("other");
                    auto sent_op = early.send(0, 2u);
                    auto sent = co_await sent_op;
                    if (!sent) co_return sent;
                    declared.store(true, std::memory_order_release);
                    auto returned_op = other.send(0, 3u);
                    co_return co_await returned_op;
                }
                auto other = group.channel<std::uint32_t>("other");
                co_await dftu_comm_test::wait_flag(declared);
                auto first_op =
                    other.recv_from(1, deadline_in(5'000'000'000ull));
                auto first = co_await first_op;
                if (!first) co_return std::move(first).error();
                CHECK(first->value() == 3u);
                auto early = group.channel<std::uint32_t>("early");
                auto second_op =
                    early.recv_from(1, deadline_in(5'000'000'000ull));
                auto second = co_await second_op;
                if (!second) co_return std::move(second).error();
                CHECK(second->value() == 2u);
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
    }

    TEST_CASE("receives in rank order cancelled oldest first keep every turn") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(3, *rt.executor());
        std::atomic<bool> cancelled{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator, &cancelled](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("turn-oldest");
                if (rank != 0) {
                    co_await dftu_comm_test::wait_flag(cancelled);
                    auto returned_op = channel.send(0, rank);
                    co_return co_await returned_op;
                }
                comm::Op<comm::Message<std::uint32_t>> first =
                    channel.recv_in_rank_order();
                comm::Op<comm::Message<std::uint32_t>> second =
                    channel.recv_in_rank_order();
                first.cancel();
                second.cancel();
                cancelled.store(true, std::memory_order_release);
                for (std::uint32_t expected = 1; expected <= 2; ++expected) {
                    auto message_op = channel.recv_in_rank_order(
                        deadline_in(5'000'000'000ull));
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    CHECK(message->from() == expected);
                }
                co_return {};
            });
        for (std::uint32_t rank = 0; rank < 3; ++rank)
            CHECK_FALSE(errors[rank].has_value());
    }
    TEST_CASE("returned rank turns come back in the order they were handed") {
        Runtime rt(4);
        comm::Communicator communicator = make_communicator(3, *rt.executor());
        std::atomic<bool> cancelled{false};
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator, &cancelled](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                auto channel = group.channel<std::uint32_t>("turn-stamps");
                if (rank != 0) {
                    co_await dftu_comm_test::wait_flag(cancelled);
                    for (std::uint32_t at = 0; at < 3 - rank; ++at) {
                        auto sent_op = channel.send(0, rank);
                        auto sent = co_await sent_op;
                        if (!sent) co_return sent;
                    }
                    co_return {};
                }
                // Turns 1, 2, 1 are handed out and given back out of order, so
                // only the hand-out order gives 1, 2, 1 again.
                comm::Op<comm::Message<std::uint32_t>> calls[3] = {
                    channel.recv_in_rank_order(), channel.recv_in_rank_order(),
                    channel.recv_in_rank_order()};
                calls[2].cancel();
                calls[0].cancel();
                calls[1].cancel();
                cancelled.store(true, std::memory_order_release);
                const std::uint32_t expected[3] = {1, 2, 1};
                for (std::uint32_t at = 0; at < 3; ++at) {
                    auto message_op = channel.recv_in_rank_order(
                        deadline_in(5'000'000'000ull));
                    auto message = co_await message_op;
                    if (!message) co_return std::move(message).error();
                    CHECK(message->from() == expected[at]);
                }
                co_return {};
            });
        for (std::uint32_t rank = 0; rank < 3; ++rank)
            CHECK_FALSE(errors[rank].has_value());
    }
}
