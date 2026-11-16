// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/support.h>
#include <dftracer/utils/comm/backend/messaging.h>
#include <dftracer/utils/comm/channel.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>

// Runs as one process of an mpirun job, so each test case has one rank here
// and the other ranks in the other processes. The backend library is loaded at
// run time, as a tool loads it.

using namespace dftracer::utils;

namespace {

comm::Communicator start_mpi() {
    auto started = comm::Communicator::start(comm::BackendKind::Mpi);
    REQUIRE_MESSAGE(started.has_value(), started.error().c_str());
    return std::move(*started);
}

template <class Body>
dstd::Optional<dstd::Error> run_here(Runtime& rt, Body body) {
    auto errors = dftu_comm_test::run_ranks(
        rt, 1, [&body](std::uint32_t) { return body(); });
    return errors[0];
}

}  // namespace

TEST_SUITE("CommMpiMessaging") {
    TEST_CASE("the communicator has one rank per process") {
        comm::Communicator communicator = start_mpi();
        const char* expected = std::getenv("DFTU_TEST_NP");
        if (expected != nullptr)
            CHECK(communicator.size() ==
                  static_cast<std::uint32_t>(std::atoi(expected)));
        CHECK(communicator.rank() < communicator.size());
        comm::Group world = communicator.world(communicator.rank());
        CHECK(world.size() == communicator.size());
        CHECK(world.rank() == communicator.rank());
    }

    TEST_CASE("a channel carries a value around the ring") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() < 2) return;
        Runtime rt(2);
        auto error = run_here(rt, [&]() -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const std::uint32_t me = group.rank();
            auto channel = group.channel<std::uint64_t>("ring");
            auto sent = co_await channel.send((me + 1) % n, 100 + me);
            if (!sent) co_return sent;
            auto message = co_await channel.recv_from((me + n - 1) % n);
            if (!message) co_return std::move(message).error();
            CHECK(message->value() == 100 + (me + n - 1) % n);
            co_return {};
        });
        CHECK_FALSE(error.has_value());
    }

    TEST_CASE("a payload of several megabytes arrives whole") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() < 2) return;
        Runtime rt(2);
        auto error = run_here(rt, [&]() -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(communicator.rank());
            auto channel = group.channel<dstd::Vector<std::byte>>("big");
            constexpr std::size_t BYTES = std::size_t{6} << 20;
            if (group.rank() == 0) {
                dstd::Vector<std::byte> bytes(BYTES);
                for (std::size_t at = 0; at < BYTES; ++at)
                    bytes[at] = static_cast<std::byte>(at * 31 + 7);
                co_return co_await channel.send(
                    1, dstd::Span<const std::byte>(bytes.data(), BYTES));
            }
            if (group.rank() == 1) {
                dstd::Vector<std::byte> into(BYTES);
                auto taken = co_await channel.recv_into(
                    dstd::Span<std::byte>(into.data(), BYTES));
                if (!taken) co_return std::move(taken).error();
                CHECK(*taken == BYTES);
                bool same = true;
                for (std::size_t at = 0; at < BYTES; ++at)
                    same =
                        same && into[at] == static_cast<std::byte>(at * 31 + 7);
                CHECK(same);
            }
            co_return {};
        });
        CHECK_FALSE(error.has_value());
    }

    TEST_CASE("a full stream refuses a send with the error of the receiver") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() < 2) return;
        Runtime rt(2);
        auto error = run_here(rt, [&]() -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(communicator.rank());
            comm::ChannelOpts opts;
            opts.depth = 1;
            opts.overflow = comm::Overflow::Fail;
            auto channel = group.channel<std::uint32_t>("full", opts);
            if (group.rank() == 0) {
                auto first = co_await channel.send(1, 1u);
                CHECK(first.has_value());
                auto second = co_await channel.send(1, 2u);
                CHECK_FALSE(second.has_value());
                if (!second)
                    CHECK(second.error().is_code(comm::ChannelError::Full));
            }
            auto joined = co_await group.barrier();
            if (!joined) co_return joined;
            if (group.rank() == 1) {
                auto message = co_await channel.recv_from(0);
                if (!message) co_return std::move(message).error();
                CHECK(message->value() == 1u);
            }
            co_return {};
        });
        CHECK_FALSE(error.has_value());
    }

    TEST_CASE("collectives run over the messages") {
        comm::Communicator communicator = start_mpi();
        Runtime rt(2);
        auto error = run_here(rt, [&]() -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(communicator.rank());
            auto joined = co_await group.barrier();
            if (!joined) co_return joined;
            const std::uint32_t mine = group.rank() * 3 + 1;
            auto all = co_await group.all_gather(
                dstd::Span<const std::uint32_t>(&mine, 1));
            if (!all) co_return std::move(all).error();
            for (std::uint32_t at = 0; at < group.size(); ++at)
                CHECK(all->as<std::uint32_t>(at)[0] == at * 3 + 1);
            co_return {};
        });
        CHECK_FALSE(error.has_value());
    }

    TEST_CASE("an abort of one rank fails the calls of every rank") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() < 2) return;
        Runtime rt(2);
        auto error = run_here(rt, [&]() -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(communicator.rank());
            if (group.rank() == 0) {
                group.abort(dstd::Error(dstd::Condition::Internal,
                                        "rank zero gave up"));
                co_return {};
            }
            auto joined = co_await group.barrier();
            CHECK_FALSE(joined.has_value());
            if (!joined) {
                CHECK(dftu_comm_test::has_text(joined.error(),
                                               "aborted by rank 0"));
                CHECK(dftu_comm_test::has_text(joined.error(),
                                               "rank zero gave up"));
                CHECK(dftu_comm_test::count_text(joined.error(),
                                                 "aborted by") == 1);
            }
            co_return {};
        });
        CHECK_FALSE(error.has_value());
    }

    TEST_CASE("a payload longer than one chunk arrives whole") {
        ::setenv("DFTRACER_UTILS_COMM_MPI_CHUNK_BYTES", "4096", 1);
        comm::Communicator communicator = start_mpi();
        ::unsetenv("DFTRACER_UTILS_COMM_MPI_CHUNK_BYTES");
        if (communicator.size() < 2) return;
        Runtime rt(2);
        auto error = run_here(rt, [&]() -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(communicator.rank());
            auto channel = group.channel<dstd::Vector<std::byte>>("cut");
            constexpr std::size_t BYTES = 100 * 1024 + 17;
            if (group.rank() == 0) {
                dstd::Vector<std::byte> bytes(BYTES);
                for (std::size_t at = 0; at < BYTES; ++at)
                    bytes[at] = static_cast<std::byte>(at * 13 + 5);
                co_return co_await channel.send(
                    1, dstd::Span<const std::byte>(bytes.data(), BYTES));
            }
            if (group.rank() == 1) {
                dstd::Vector<std::byte> into(BYTES);
                auto taken = co_await channel.recv_into(
                    dstd::Span<std::byte>(into.data(), BYTES));
                if (!taken) co_return std::move(taken).error();
                CHECK(*taken == BYTES);
                bool same = true;
                for (std::size_t at = 0; at < BYTES; ++at)
                    same =
                        same && into[at] == static_cast<std::byte>(at * 13 + 5);
                CHECK(same);
            }
            co_return {};
        });
        CHECK_FALSE(error.has_value());
    }

    TEST_CASE(
        "the last owner of a communicator may be a thread of the "
        "backend") {
        // An abort in flight while the communicator goes: the thread that
        // handles it may hold the last reference, and must not wait for or
        // join itself.
        for (int round = 0; round < 25; ++round) {
            comm::Communicator communicator = start_mpi();
            if (communicator.size() < 2) return;
            comm::Group group = communicator.world(communicator.rank());
            if (group.rank() == 0)
                group.abort(dstd::Error(dstd::Condition::Internal, "going"));
            // The group handle and the communicator go here, with the abort
            // possibly still on its way to the other ranks.
        }
        CHECK(true);
    }

    TEST_CASE("a remote cancel ends the send only when the receiver held it") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() != 2) return;
        auto messaging = comm::detail::Hooks::messaging(communicator);
        const std::uint32_t me = communicator.rank();
        comm::backend::StreamOpts one;
        one.depth = 1;
        one.overflow = comm::Overflow::Block;
        auto key = [](std::uint32_t source, std::uint64_t tag) {
            return comm::backend::MessageKey{900, source, 1 - source, tag};
        };
        auto bytes = [](std::byte value) {
            dstd::Vector<std::byte> out(8);
            for (auto& byte : out) byte = value;
            return comm::backend::Payload(std::move(out));
        };
        if (me == 0) {
            std::atomic<int> first{0};
            std::atomic<int> second{0};
            std::atomic<bool> second_failed{false};
            messaging->send(key(0, 1), bytes(std::byte{1}), one,
                            [&](const dstd::Error* why) {
                                first.store(why == nullptr ? 1 : 2);
                            });
            REQUIRE(
                dftu_comm_test::spin_until([&] { return first.load() != 0; }));
            CHECK(first.load() == 1);
            // The stream holds its one message, so the second parks at the
            // receiver, which still holds it when the cancel arrives.
            const auto parked =
                messaging->send(key(0, 1), bytes(std::byte{2}), one,
                                [&](const dstd::Error* why) {
                                    second_failed.store(why != nullptr);
                                    second.store(1);
                                });
            messaging->cancel(
                key(0, 1), parked,
                dstd::Error(dstd::Condition::Cancelled, "test cancel"));
            REQUIRE(
                dftu_comm_test::spin_until([&] { return second.load() != 0; }));
            CHECK(second_failed.load());
        }
        // Rank 1 does not take the first message until the cancel is over.
        std::atomic<int> got{0};
        comm::Group world = communicator.world(me);
        Runtime rt(2);
        auto error = run_here(rt, [&]() -> coro::CoroTask<dstd::Result<void>> {
            co_return co_await world.barrier();
        });
        CHECK_FALSE(error.has_value());
        if (me == 1) {
            messaging->recv(key(0, 1), [&](dstd::Span<const std::byte> taken,
                                           const dstd::Error* why) {
                CHECK(why == nullptr);
                CHECK(taken.size() == 8);
                CHECK(taken[0] == std::byte{1});
                got.store(1);
            });
            REQUIRE(
                dftu_comm_test::spin_until([&] { return got.load() != 0; }));
        }
    }

    TEST_CASE("a cancel that loses to the delivery leaves the send done") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() != 2) return;
        auto messaging = comm::detail::Hooks::messaging(communicator);
        const std::uint32_t me = communicator.rank();
        const comm::backend::MessageKey key{901, me, 1 - me, 7};
        std::atomic<int> received{0};
        if (me == 1) {
            // A receive waits, so the message is delivered when it arrives.
            messaging->recv(
                comm::backend::MessageKey{901, 0, 1, 7},
                [&](dstd::Span<const std::byte>, const dstd::Error* why) {
                    received.store(why == nullptr ? 1 : 2);
                });
        }
        comm::Group world = communicator.world(me);
        Runtime rt(2);
        auto ready = run_here(rt, [&]() -> coro::CoroTask<dstd::Result<void>> {
            co_return co_await world.barrier();
        });
        CHECK_FALSE(ready.has_value());
        if (me == 0) {
            std::atomic<int> done{0};
            std::atomic<bool> failed{true};
            comm::backend::StreamOpts opts;
            opts.depth = 4;
            const auto id = messaging->send(
                key, comm::backend::Payload(dstd::Vector<std::byte>(8)), opts,
                [&](const dstd::Error* why) {
                    failed.store(why != nullptr);
                    done.store(1);
                });
            messaging->cancel(
                key, id, dstd::Error(dstd::Condition::Cancelled, "too late"));
            REQUIRE(
                dftu_comm_test::spin_until([&] { return done.load() != 0; }));
            CHECK_FALSE(failed.load());
        } else {
            REQUIRE(dftu_comm_test::spin_until(
                [&] { return received.load() != 0; }));
            CHECK(received.load() == 1);
        }
    }
}
