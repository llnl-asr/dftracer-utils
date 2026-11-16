// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/support.h>
#include <dftracer/utils/comm/backend/linear_collectives.h>
#include <dftracer/utils/comm/channel.h>
#include <dftracer/utils/comm/codec.h>
#include <dftracer/utils/comm/collective.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>

// Runs as one process of an mpirun job. Every call goes through the MPI
// backend, loaded at run time, and each test reads the counters of the native
// provider, so a result that came out right over the linear fallback fails:
// run with DFTU_TEST_LINEAR=1 to see the counters fail.

using namespace dftracer::utils;

namespace {

using Task = coro::CoroTask<dstd::Result<void>>;
using comm::detail::CallKind;

comm::Communicator start_mpi() {
    auto started = comm::Communicator::start(comm::BackendKind::Mpi);
    if (!started) FAIL(started.error().c_str());
    comm::Communicator communicator = std::move(*started);
    // A run with DFTU_TEST_LINEAR=1 uses the collectives over the messages, so
    // the counters of the native provider must then fail.
    if (std::getenv("DFTU_TEST_LINEAR") != nullptr)
        comm::detail::Hooks::set_collectives(
            communicator, comm::backend::make_linear_collectives(
                              comm::detail::Hooks::messaging(communicator)));
    return communicator;
}

std::string text_of(const dstd::Error& error) {
    const dstd::StringView message = error.message();
    return std::string(message.data(), message.size());
}

template <class Body>
void run_one(Body body) {
    Runtime rt(2);
    auto errors = dftu_comm_test::run_ranks(
        rt, 1, [&body](std::uint32_t) { return body(); });
    CHECK_FALSE(errors[0].has_value());
    if (errors[0]) MESSAGE(text_of(*errors[0]));
}

std::uint64_t native_calls(const comm::Communicator& communicator,
                           CallKind kind) {
    return comm::detail::Hooks::collectives(communicator)
        ->native_slots(static_cast<std::uint32_t>(kind));
}

std::uint64_t native_folds(const comm::Communicator& communicator) {
    return comm::detail::Hooks::collectives(communicator)->native_folds();
}

std::uint32_t bits_of(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

dstd::Span<const std::byte> view_of(const dstd::Vector<std::byte>& bytes) {
    return dstd::Span<const std::byte>(bytes.data(), bytes.size());
}

dstd::Vector<std::byte> filled(std::size_t size, std::uint32_t value) {
    return dstd::Vector<std::byte>(size, static_cast<std::byte>(value));
}

bool holds(dstd::Span<const std::byte> bytes, std::size_t size,
           std::uint32_t value) {
    if (bytes.size() != size) return false;
    for (const std::byte byte : bytes)
        if (byte != static_cast<std::byte>(value)) return false;
    return true;
}

}  // namespace

TEST_SUITE("CommMpiCollectives") {
    TEST_CASE("a barrier ends on the records of every rank") {
        comm::Communicator communicator = start_mpi();
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            for (int round = 0; round < 5; ++round) {
                auto joined = co_await group.barrier();
                if (!joined) co_return joined;
            }
            auto monitored = co_await group.monitored_barrier();
            if (!monitored) co_return monitored;
            co_return {};
        });
        CHECK(native_calls(communicator, CallKind::Barrier) == 5);
        CHECK(native_calls(communicator, CallKind::MonitoredBarrier) == 1);
    }

    TEST_CASE("an integer sum runs MPI_Iallreduce and the group merges none") {
        comm::Communicator communicator = start_mpi();
        const std::uint64_t before = native_folds(communicator);
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            auto total = co_await group.all_reduce(
                static_cast<std::int64_t>(group.rank() + 1),
                comm::ReduceOp::Sum);
            if (!total) co_return std::move(total).error();
            CHECK(*total == static_cast<std::int64_t>(n) * (n + 1) / 2);
            auto smallest = co_await group.all_reduce(
                static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(group.rank() + 7)),
                comm::ReduceOp::Min);
            if (!smallest) co_return std::move(smallest).error();
            CHECK(*smallest == 7);
            auto largest = co_await group.all_reduce(
                static_cast<std::int8_t>(
                    static_cast<std::int8_t>(group.rank() - 3)),
                comm::ReduceOp::Max);
            if (!largest) co_return std::move(largest).error();
            CHECK(*largest == static_cast<std::int8_t>(n) - 4);
            const dstd::Vector<std::int32_t> values = {1, 2, 3, 4, 5};
            auto sums = co_await group.all_reduce(
                dstd::Span<const std::int32_t>(values.data(), values.size()),
                comm::ReduceOp::Sum);
            if (!sums) co_return std::move(sums).error();
            CHECK(sums->size() == 5);
            for (std::size_t at = 0; at < sums->size(); ++at)
                CHECK((*sums)[at] == static_cast<std::int32_t>(n * values[at]));
            co_return {};
        });
        CHECK(native_calls(communicator, CallKind::AllReduce) == 4);
        CHECK(native_folds(communicator) - before == 4);
    }

    TEST_CASE("a float sum in rank order equals the serial fold bit for bit") {
        comm::Communicator communicator = start_mpi();
        const std::uint64_t before = native_folds(communicator);
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            // Any other order than 0, 1, 2, 3 gives another sum.
            const float table[4] = {1e8f, 1.0f, -1e8f, 3.0f};
            float expected = table[0];
            for (std::uint32_t rank = 1; rank < n; ++rank)
                expected += table[rank];
            auto total = co_await group.all_reduce(table[group.rank()],
                                                   comm::ReduceOp::Sum);
            if (!total) co_return std::move(total).error();
            CHECK(bits_of(*total) == bits_of(expected));
            co_return {};
        });
        // The group folded it: the provider moved the contributions only.
        CHECK(native_folds(communicator) == before);
        CHECK(native_calls(communicator, CallKind::AllReduce) == 1);
    }

    TEST_CASE("float min and any-order float sums") {
        comm::Communicator communicator = start_mpi();
        const std::uint64_t before = native_folds(communicator);
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const float mine = static_cast<float>(group.rank()) * 1.5f - 2.0f;
            auto smallest =
                co_await group.all_reduce(mine, comm::ReduceOp::Min);
            if (!smallest) co_return std::move(smallest).error();
            CHECK(*smallest == -2.0f);
            comm::CallOpts any;
            any.reduce_order = comm::ReduceOrder::Any;
            auto total =
                co_await group.all_reduce(static_cast<double>(group.rank() + 1),
                                          comm::ReduceOp::Sum, any);
            if (!total) co_return std::move(total).error();
            CHECK(*total == static_cast<double>(n) * (n + 1) / 2);
            co_return {};
        });
        // Only the any-order sum is folded by MPI.
        CHECK(native_folds(communicator) - before == 1);
    }

    TEST_CASE("a custom merge folds in rank order") {
        comm::Communicator communicator = start_mpi();
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            auto joined = co_await group.all_reduce(
                static_cast<std::uint64_t>(group.rank() + 1),
                [](const std::uint64_t& a, const std::uint64_t& b) {
                    return a * 10 + b;
                });
            if (!joined) co_return std::move(joined).error();
            std::uint64_t expected = 1;
            for (std::uint32_t rank = 1; rank < n; ++rank)
                expected = expected * 10 + rank + 1;
            CHECK(*joined == expected);
            co_return {};
        });
    }

    TEST_CASE("reduce gives the root the merge and the others nothing") {
        comm::Communicator communicator = start_mpi();
        const std::uint64_t before = native_folds(communicator);
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const dstd::Vector<std::int64_t> values = {
                static_cast<std::int64_t>(group.rank()), 10};
            const std::uint32_t roots[2] = {0, n - 1};
            for (const std::uint32_t root : roots) {
                auto sums =
                    co_await group.reduce(root,
                                          dstd::Span<const std::int64_t>(
                                              values.data(), values.size()),
                                          comm::ReduceOp::Sum);
                if (!sums) co_return std::move(sums).error();
                if (group.rank() == root) {
                    CHECK(sums->size() == 2);
                    CHECK((*sums)[0] ==
                          static_cast<std::int64_t>(n) * (n - 1) / 2);
                    CHECK((*sums)[1] == static_cast<std::int64_t>(n) * 10);
                } else {
                    CHECK(sums->empty());
                }
            }
            auto one = co_await group.reduce(
                n - 1, static_cast<std::int32_t>(3), comm::ReduceOp::Max);
            if (!one) co_return std::move(one).error();
            if (group.rank() == n - 1) CHECK(*one == 3);
            co_return {};
        });
        CHECK(native_calls(communicator, CallKind::Reduce) == 3);
        CHECK(native_folds(communicator) - before == 3);
    }

    TEST_CASE("all_gather keeps one buffer and a view per rank") {
        comm::Communicator communicator = start_mpi();
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const std::uint32_t me = group.rank();
            // Equal sizes.
            const std::uint32_t mine = me * 3 + 1;
            auto equal = co_await group.all_gather(
                dstd::Span<const std::uint32_t>(&mine, 1));
            if (!equal) co_return std::move(equal).error();
            for (std::uint32_t at = 0; at < n; ++at)
                CHECK(equal->as<std::uint32_t>(at)[0] == at * 3 + 1);
            CHECK(equal->data().size() == n * sizeof(std::uint32_t));
            // Unequal sizes, rank 0 empty.
            const dstd::Vector<std::byte> block = filled(me * 3, me + 1);
            auto unequal = co_await group.all_gather(view_of(block));
            if (!unequal) co_return std::move(unequal).error();
            for (std::uint32_t at = 0; at < n; ++at)
                CHECK(holds((*unequal)[at], at * 3, at + 1));
            // Every contribution empty.
            auto none =
                co_await group.all_gather(dstd::Span<const std::byte>());
            if (!none) co_return std::move(none).error();
            CHECK(none->size() == n);
            for (std::uint32_t at = 0; at < n; ++at) CHECK((*none)[at].empty());
            co_return {};
        });
        CHECK(native_calls(communicator, CallKind::AllGather) == 3);
    }

    TEST_CASE("gather, scatter and broadcast with roots first and last") {
        comm::Communicator communicator = start_mpi();
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const std::uint32_t me = group.rank();
            const std::uint32_t roots[2] = {0, n - 1};
            for (const std::uint32_t root : roots) {
                const dstd::Vector<std::byte> mine = filled(me + 2, me + 1);
                auto gathered = co_await group.gather(root, view_of(mine));
                if (!gathered) co_return std::move(gathered).error();
                if (me == root) {
                    CHECK(gathered->size() == n);
                    for (std::uint32_t at = 0; at < n; ++at)
                        CHECK(holds((*gathered)[at], at + 2, at + 1));
                } else {
                    CHECK(gathered->size() == 0);
                }

                dstd::Vector<dstd::Vector<std::byte>> blocks;
                dstd::Vector<dstd::Span<const std::byte>> pieces;
                if (me == root) {
                    for (std::uint32_t dest = 0; dest < n; ++dest)
                        blocks.push_back(filled(dest + 1, dest + 40));
                    for (const auto& block : blocks)
                        pieces.push_back(view_of(block));
                }
                auto piece = co_await group.scatter(
                    root, dstd::Span<const dstd::Span<const std::byte>>(
                              pieces.data(), pieces.size()));
                if (!piece) co_return std::move(piece).error();
                CHECK(holds(piece->view, me + 1, me + 40));

                const dstd::Vector<std::byte> value = filled(70000, root + 9);
                auto shared = co_await group.broadcast(
                    root, me == root ? view_of(value)
                                     : dstd::Span<const std::byte>());
                if (!shared) co_return std::move(shared).error();
                CHECK(holds(shared->view, 70000, root + 9));
            }
            auto empty =
                co_await group.broadcast(0, dstd::Span<const std::byte>());
            if (!empty) co_return std::move(empty).error();
            CHECK(empty->view.empty());
            auto typed =
                co_await group.broadcast(n - 1, static_cast<std::uint64_t>(77));
            if (!typed) co_return std::move(typed).error();
            CHECK(*typed == 77);
            co_return {};
        });
        CHECK(native_calls(communicator, CallKind::Gather) == 2);
        CHECK(native_calls(communicator, CallKind::Scatter) == 2);
        CHECK(native_calls(communicator, CallKind::Broadcast) == 4);
    }

    TEST_CASE("all_to_all, exchange_by and reduce_scatter") {
        comm::Communicator communicator = start_mpi();
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const std::uint32_t me = group.rank();
            dstd::Vector<dstd::Vector<std::byte>> blocks;
            for (std::uint32_t dest = 0; dest < n; ++dest)
                blocks.push_back(filled((me + dest) % 3, me * 4 + dest));
            dstd::Vector<dstd::Span<const std::byte>> views;
            for (const auto& block : blocks) views.push_back(view_of(block));
            auto exchanged = co_await group.all_to_all(
                dstd::Span<const dstd::Span<const std::byte>>(views.data(),
                                                              views.size()));
            if (!exchanged) co_return std::move(exchanged).error();
            for (std::uint32_t from = 0; from < n; ++from)
                CHECK(
                    holds((*exchanged)[from], (from + me) % 3, from * 4 + me));

            dstd::Vector<std::uint32_t> rows;
            for (std::uint32_t at = 0; at < 10; ++at)
                rows.push_back(me * 100 + at);
            auto by_rank = [n](std::uint32_t row) { return row % n; };
            auto routed = co_await group.exchange_by(
                dstd::Span<const std::uint32_t>(rows.data(), rows.size()),
                by_rank);
            if (!routed) co_return std::move(routed).error();
            for (std::uint32_t from = 0; from < n; ++from) {
                dstd::Vector<std::uint32_t> expected;
                for (std::uint32_t at = 0; at < 10; ++at)
                    if ((from * 100 + at) % n == me)
                        expected.push_back(from * 100 + at);
                const auto got = routed->as<std::uint32_t>(from);
                CHECK(got.size() == expected.size());
                for (std::size_t at = 0;
                     at < got.size() && at < expected.size(); ++at)
                    CHECK(got[at] == expected[at]);
            }

            dstd::Vector<std::int32_t> values;
            for (std::uint32_t at = 0; at < n * 2; ++at)
                values.push_back(static_cast<std::int32_t>(me + at));
            auto block = co_await group.reduce_scatter(
                dstd::Span<const std::int32_t>(values.data(), values.size()),
                comm::ReduceOp::Sum);
            if (!block) co_return std::move(block).error();
            CHECK(block->size() == 2);
            for (std::uint32_t at = 0; at < 2; ++at) {
                std::int32_t expected = 0;
                for (std::uint32_t rank = 0; rank < n; ++rank)
                    expected += static_cast<std::int32_t>(rank + me * 2 + at);
                CHECK((*block)[at] == expected);
            }
            co_return {};
        });
        CHECK(native_calls(communicator, CallKind::AllToAll) == 2);
        CHECK(native_calls(communicator, CallKind::ReduceScatter) == 1);
    }

    TEST_CASE("scan, exclusive_scan and neighbor_exchange") {
        comm::Communicator communicator = start_mpi();
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const std::uint32_t me = group.rank();
            auto prefix = co_await group.scan(static_cast<std::int64_t>(me + 1),
                                              comm::ReduceOp::Sum);
            if (!prefix) co_return std::move(prefix).error();
            CHECK(*prefix == static_cast<std::int64_t>(me + 1) * (me + 2) / 2);
            auto before = co_await group.exclusive_scan(
                static_cast<std::int64_t>(me + 1), comm::ReduceOp::Sum);
            if (!before) co_return std::move(before).error();
            if (me == 0)
                CHECK_FALSE(before->has_value());
            else
                CHECK(**before == static_cast<std::int64_t>(me) * (me + 1) / 2);

            const std::uint32_t next = (me + 1) % n;
            auto graph = co_await group.neighbors(
                dstd::Span<const std::uint32_t>(&next, 1));
            if (!graph) co_return std::move(graph).error();
            const std::uint64_t sent = me * 10 + 5;
            auto blocks = co_await graph->neighbor_exchange(
                dstd::Span<const std::uint64_t>(&sent, 1));
            if (!blocks) co_return std::move(blocks).error();
            CHECK(blocks->size() == 1);
            CHECK(blocks->source(0) == (me + n - 1) % n);
            CHECK(blocks->as<std::uint64_t>(0)[0] ==
                  ((me + n - 1) % n) * 10 + 5);
            co_return {};
        });
        CHECK(native_calls(communicator, CallKind::Scan) == 1);
        CHECK(native_calls(communicator, CallKind::ExclusiveScan) == 1);
        CHECK(native_calls(communicator, CallKind::NeighborExchange) == 1);
    }

    TEST_CASE("object calls carry strings through the codec") {
        comm::Communicator communicator = start_mpi();
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const std::uint32_t me = group.rank();
            const char* names[4] = {"alpha", "beta", "gamma", "delta"};
            const comm::StringCodec codec;
            auto everyone = co_await group.all_gather_object(
                dstd::String(names[me % 4]), codec);
            if (!everyone) co_return std::move(everyone).error();
            CHECK(everyone->size() == n);
            for (std::uint32_t at = 0; at < n; ++at)
                CHECK(*(*everyone)[at] == dstd::String(names[at % 4]));

            auto rooted = co_await group.gather_object(
                n - 1, dstd::String(names[me % 4]), codec);
            if (!rooted) co_return std::move(rooted).error();
            if (me == n - 1) {
                CHECK(rooted->size() == n);
                CHECK(*(*rooted)[0] == dstd::String(names[0]));
            }

            auto shared = co_await group.broadcast_object(
                0, dstd::String(names[me % 4]), codec);
            if (!shared) co_return std::move(shared).error();
            CHECK(**shared == dstd::String(names[0]));

            dstd::Vector<dstd::String> out;
            for (std::uint32_t dest = 0; dest < n; ++dest)
                out.push_back(dstd::String(names[(me + dest) % 4]));
            auto swapped =
                co_await group.all_to_all_object(std::move(out), codec);
            if (!swapped) co_return std::move(swapped).error();
            CHECK(swapped->size() == n);
            for (std::uint32_t from = 0; from < n; ++from)
                CHECK(*(*swapped)[from] ==
                      dstd::String(names[(from + me) % 4]));
            co_return {};
        });
        CHECK(native_calls(communicator, CallKind::AllToAll) == 1);
    }

    TEST_CASE("a channel carries a value around the ring") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() < 2) return;
        run_one([&]() -> Task {
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
    }

    TEST_CASE("split makes groups that run native collectives and channels") {
        comm::Communicator communicator = start_mpi();
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const std::uint32_t me = group.rank();
            auto split = co_await group.split(me % 2, me);
            if (!split) co_return std::move(split).error();
            comm::Group child = *split;
            const std::uint32_t others = (n - me % 2 + 1) / 2;
            CHECK(child.size() == others);
            CHECK(child.rank() == me / 2);
            auto joined = co_await child.barrier();
            if (!joined) co_return joined;
            auto total = co_await child.all_reduce(
                static_cast<std::int64_t>(me), comm::ReduceOp::Sum);
            if (!total) co_return std::move(total).error();
            std::int64_t expected = 0;
            for (std::uint32_t rank = me % 2; rank < n; rank += 2)
                expected += rank;
            CHECK(*total == expected);
            auto world = co_await child.all_gather(
                dstd::Span<const std::uint32_t>(&me, 1));
            if (!world) co_return std::move(world).error();
            for (std::uint32_t at = 0; at < child.size(); ++at)
                CHECK(world->as<std::uint32_t>(at)[0] == me % 2 + at * 2);

            const std::uint32_t k = child.size();
            if (k < 2) co_return {};
            auto channel = child.channel<std::uint64_t>("child ring");
            const std::uint32_t mine = child.rank();
            auto sent = co_await channel.send((mine + 1) % k, 500 + me);
            if (!sent) co_return sent;
            auto message = co_await channel.recv_from((mine + k - 1) % k);
            if (!message) co_return std::move(message).error();
            const std::uint32_t before_world =
                me % 2 + ((mine + k - 1) % k) * 2;
            CHECK(message->value() == 500 + before_world);
            co_return {};
        });
    }

    TEST_CASE("a split by a reversed key orders the new ranks by it") {
        comm::Communicator communicator = start_mpi();
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const std::uint32_t me = group.rank();
            auto split = co_await group.split(0, n - 1 - me);
            if (!split) co_return std::move(split).error();
            comm::Group child = *split;
            CHECK(child.rank() == n - 1 - me);
            auto world = co_await child.all_gather(
                dstd::Span<const std::uint32_t>(&me, 1));
            if (!world) co_return std::move(world).error();
            for (std::uint32_t at = 0; at < n; ++at)
                CHECK(world->as<std::uint32_t>(at)[0] == n - 1 - at);
            auto root =
                co_await child.broadcast(0, static_cast<std::uint32_t>(me));
            if (!root) co_return std::move(root).error();
            CHECK(*root == n - 1);
            co_return {};
        });
    }

    TEST_CASE("sixty splits in a row each run their collectives") {
        comm::Communicator communicator = start_mpi();
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t me = group.rank();
            for (int round = 0; round < 60; ++round) {
                auto split = co_await group.split(me % 2, me);
                if (!split) co_return std::move(split).error();
                auto total = co_await split->all_reduce(
                    static_cast<std::int32_t>(1), comm::ReduceOp::Sum);
                if (!total) co_return std::move(total).error();
                CHECK(*total == static_cast<std::int32_t>(split->size()));
            }
            co_return {};
        });
    }

    TEST_CASE("three thousand split groups are made, used and dropped") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() != 2) return;
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            for (int round = 0; round < 3000; ++round) {
                auto split = co_await group.split(0, group.rank());
                if (!split) co_return std::move(split).error();
                auto total = co_await split->all_reduce(
                    static_cast<std::int32_t>(1), comm::ReduceOp::Sum);
                if (!total) co_return std::move(total).error();
                CHECK(*total == 2);
            }
            co_return {};
        });
    }

    TEST_CASE(
        "splits in a row give every child its members, used in any order") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() != 4) return;
        run_one([&]() -> Task {
            comm::Group world = communicator.world(communicator.rank());
            const std::uint32_t me = world.rank();
            // Child c of split k holds the ranks that share (rank / 2) % 2
            // for k 0, rank % 2 for k 1 and rank < 2 ? 0 : 1 for k 2.
            const std::uint32_t colors[3] = {(me / 2) % 2, me % 2,
                                             me < 2 ? 0u : 1u};
            dstd::Vector<comm::Op<comm::Group>> ops;
            for (int k = 0; k < 3; ++k)
                ops.push_back(world.split(colors[k], me));
            comm::Group kids[3];
            for (int k = 0; k < 3; ++k) {
                auto made = co_await ops[k];
                if (!made) co_return std::move(made).error();
                kids[k] = *made;
            }
            // Each child sums the world ranks of its members.
            std::int32_t expected[3];
            for (int k = 0; k < 3; ++k) {
                expected[k] = 0;
                for (std::uint32_t rank = 0; rank < 4; ++rank) {
                    const std::uint32_t colour[3] = {(rank / 2) % 2, rank % 2,
                                                     rank < 2 ? 0u : 1u};
                    if (colour[k] == colors[k])
                        expected[k] += static_cast<std::int32_t>(rank);
                }
            }
            // Start the calls in an order that depends on the rank, then wait.
            int order[3] = {0, 1, 2};
            if (me % 2 == 1) {
                order[0] = 2;
                order[2] = 0;
            }
            dstd::Vector<comm::Op<std::int32_t>> sums;
            for (const int k : order)
                sums.push_back(kids[k].all_reduce(static_cast<std::int32_t>(me),
                                                  comm::ReduceOp::Sum));
            for (int at = 0; at < 3; ++at) {
                auto total = co_await sums[at];
                if (!total) co_return std::move(total).error();
                CHECK(*total == expected[order[at]]);
                CHECK(kids[order[at]].size() == 2);
            }
            co_return {};
        });
    }

    TEST_CASE("the node group runs native collectives") {
        comm::Communicator communicator = start_mpi();
        run_one([&]() -> Task {
            comm::Group node = communicator.local(communicator.rank());
            auto total = co_await node.all_reduce(static_cast<std::int32_t>(1),
                                                  comm::ReduceOp::Sum);
            if (!total) co_return std::move(total).error();
            CHECK(*total == static_cast<std::int32_t>(node.size()));
            co_return {};
        });
    }

    TEST_CASE("two groups with the same members stay apart in any order") {
        comm::Communicator communicator = start_mpi();
        comm::Group first = communicator.world(communicator.rank());
        comm::Group second;
        run_one([&]() -> Task {
            auto split = co_await first.split(0, first.rank());
            if (!split) co_return std::move(split).error();
            second = *split;
            co_return {};
        });
        Runtime rt(4);
        auto errors =
            dftu_comm_test::run_ranks(rt, 2, [&](std::uint32_t which) -> Task {
                comm::Group group = which == 0 ? first : second;
                const std::uint32_t n = group.size();
                const std::uint32_t me = group.rank();
                // Ranks start the two streams in opposite orders.
                const std::uint32_t lag = which == 0 ? me : n - me;
                for (std::uint32_t turn = 0; turn < lag * 3; ++turn)
                    co_await coro::yield();
                for (std::int64_t round = 0; round < 40; ++round) {
                    const std::int64_t mine = (me + 1) * (which + 1) + round;
                    auto total =
                        co_await group.all_reduce(mine, comm::ReduceOp::Sum);
                    if (!total) co_return std::move(total).error();
                    std::int64_t expected = 0;
                    for (std::uint32_t rank = 0; rank < n; ++rank)
                        expected += (rank + 1) * (which + 1) + round;
                    CHECK(*total == expected);
                }
                co_return {};
            });
        for (const auto& error : errors) {
            CHECK_FALSE(error.has_value());
            if (error) MESSAGE(text_of(*error));
        }
    }

    TEST_CASE("ranks that name different calls fail with the two calls") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() < 2) return;
        Runtime rt(2);
        auto errors =
            dftu_comm_test::run_ranks(rt, 1, [&](std::uint32_t) -> Task {
                comm::Group group = communicator.world(communicator.rank());
                if (group.rank() == 0) {
                    auto joined = co_await group.barrier();
                    CHECK_FALSE(joined.has_value());
                    if (!joined)
                        CHECK(
                            dftu_comm_test::has_text(joined.error(), "called"));
                } else {
                    auto total = co_await group.all_reduce(
                        static_cast<std::int32_t>(1), comm::ReduceOp::Sum);
                    CHECK_FALSE(total.has_value());
                    if (!total)
                        CHECK(
                            dftu_comm_test::has_text(total.error(), "called"));
                }
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
    }

    TEST_CASE("ranks that give another count fail with the shape error") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() < 2) return;
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const dstd::Vector<std::int32_t> values(group.rank() == 0 ? 2 : 3,
                                                    1);
            auto total = co_await group.all_reduce(
                dstd::Span<const std::int32_t>(values.data(), values.size()),
                comm::ReduceOp::Sum);
            CHECK_FALSE(total.has_value());
            if (!total) CHECK(dftu_comm_test::has_text(total.error(), "count"));
            co_return {};
        });
    }

    TEST_CASE("a malformed personal block fails every rank with one error") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() < 2) return;
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            // Rank 1 gives three bytes, which cannot hold the n sizes.
            dstd::Vector<std::byte> bad(3, std::byte{1});
            comm::backend::Contribution contribution;
            if (group.rank() == 1) {
                auto block = dstd::make_shared<dstd::Vector<std::byte>>(bad);
                const dstd::Span<const std::byte> view(block->data(),
                                                       block->size());
                contribution =
                    comm::backend::Contribution{view, std::move(block), {}};
            } else {
                contribution = comm::detail::own_personal(n, [](std::uint32_t) {
                    return dstd::Span<const std::byte>();
                });
            }
            auto op = comm::detail::Hooks::all_to_all_raw(
                group, std::move(contribution));
            auto got = co_await op;
            CHECK_FALSE(got.has_value());
            if (!got) {
                CHECK(dftu_comm_test::has_text(
                    got.error(), "rank 1 does not hold one piece"));
                CHECK(dftu_comm_test::count_text(got.error(), "aborted by") ==
                      1);
            }
            co_return {};
        });
    }

    TEST_CASE("calls move in rounds when the limit is small") {
        comm::Communicator communicator = start_mpi();
        comm::detail::Hooks::set_chunk_limit(communicator, 7);
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const std::uint32_t me = group.rank();
            // Unequal sizes over several rounds, rank 0 empty.
            const dstd::Vector<std::byte> block = filled(me * 13, me + 1);
            auto unequal = co_await group.all_gather(view_of(block));
            if (!unequal) co_return std::move(unequal).error();
            for (std::uint32_t at = 0; at < n; ++at)
                CHECK(holds((*unequal)[at], at * 13, at + 1));
            // Equal sizes.
            const dstd::Vector<std::byte> same = filled(30, me + 5);
            auto equal = co_await group.all_gather(view_of(same));
            if (!equal) co_return std::move(equal).error();
            for (std::uint32_t at = 0; at < n; ++at)
                CHECK(holds((*equal)[at], 30, at + 5));
            const std::uint32_t roots[2] = {0, n - 1};
            for (const std::uint32_t root : roots) {
                auto gathered = co_await group.gather(root, view_of(block));
                if (!gathered) co_return std::move(gathered).error();
                if (me == root)
                    for (std::uint32_t at = 0; at < n; ++at)
                        CHECK(holds((*gathered)[at], at * 13, at + 1));
                const dstd::Vector<std::byte> value = filled(100, root + 3);
                auto shared = co_await group.broadcast(
                    root, me == root ? view_of(value)
                                     : dstd::Span<const std::byte>());
                if (!shared) co_return std::move(shared).error();
                CHECK(holds(shared->view, 100, root + 3));
                dstd::Vector<dstd::Vector<std::byte>> blocks;
                dstd::Vector<dstd::Span<const std::byte>> pieces;
                if (me == root) {
                    for (std::uint32_t dest = 0; dest < n; ++dest)
                        blocks.push_back(filled(dest * 11 + 1, dest + 20));
                    for (const auto& piece : blocks)
                        pieces.push_back(view_of(piece));
                }
                auto mine = co_await group.scatter(
                    root, dstd::Span<const dstd::Span<const std::byte>>(
                              pieces.data(), pieces.size()));
                if (!mine) co_return std::move(mine).error();
                CHECK(holds(mine->view, me * 11 + 1, me + 20));
            }
            dstd::Vector<dstd::Vector<std::byte>> blocks;
            for (std::uint32_t dest = 0; dest < n; ++dest)
                blocks.push_back(filled((me + dest) * 9, me * 4 + dest));
            dstd::Vector<dstd::Span<const std::byte>> views;
            for (const auto& piece : blocks) views.push_back(view_of(piece));
            auto exchanged = co_await group.all_to_all(
                dstd::Span<const dstd::Span<const std::byte>>(views.data(),
                                                              views.size()));
            if (!exchanged) co_return std::move(exchanged).error();
            for (std::uint32_t from = 0; from < n; ++from)
                CHECK(
                    holds((*exchanged)[from], (from + me) * 9, from * 4 + me));
            dstd::Vector<std::int32_t> values;
            for (std::int32_t at = 0; at < 40; ++at)
                values.push_back(static_cast<std::int32_t>(me) + at);
            auto sums = co_await group.all_reduce(
                dstd::Span<const std::int32_t>(values.data(), values.size()),
                comm::ReduceOp::Sum);
            if (!sums) co_return std::move(sums).error();
            for (std::int32_t at = 0; at < 40; ++at)
                CHECK((*sums)[at] ==
                      static_cast<std::int32_t>(n * (n - 1) / 2 + n * at));
            co_return {};
        });
    }

    TEST_CASE(
        "a split of a subgroup keeps its own collectives, channels and abort") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() != 4) return;
        run_one([&]() -> Task {
            comm::Group world = communicator.world(communicator.rank());
            const std::uint32_t me = world.rank();
            auto first = co_await world.split(me % 2, me);
            if (!first) co_return std::move(first).error();
            auto second = co_await first->split(0, me);
            if (!second) co_return std::move(second).error();
            comm::Group grand = *second;
            CHECK(grand.size() == 2);
            auto channel = grand.channel<std::uint64_t>("grand");
            const std::uint32_t peer = 1 - grand.rank();
            auto sent = co_await channel.send(peer, 700 + me);
            if (!sent) co_return sent;
            auto message = co_await channel.recv_from(peer);
            if (!message) co_return std::move(message).error();
            CHECK(message->value() == 700 + (me + 2) % 4);
            if (me == 0) {
                // Abort the grandchild of {0, 2}: the other pair must go on.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                grand.abort(
                    dstd::Error(dstd::Condition::Internal, "pair gave up"));
            } else if (me == 2) {
                auto waiting = grand.channel<std::uint64_t>("never");
                auto got = co_await waiting.recv_from(0);
                CHECK_FALSE(got.has_value());
                if (!got)
                    CHECK(
                        dftu_comm_test::has_text(got.error(), "pair gave up"));
            } else {
                for (int round = 0; round < 30000; ++round) {
                    auto total = co_await grand.all_reduce(
                        static_cast<std::int32_t>(1), comm::ReduceOp::Sum);
                    if (!total) co_return std::move(total).error();
                    CHECK(*total == 2);
                }
            }
            co_return {};
        });
    }

    TEST_CASE("a signed integer sum wraps as the rank-order fold does") {
        comm::Communicator communicator = start_mpi();
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const std::int32_t mine =
                std::numeric_limits<std::int32_t>::max() - 1 +
                static_cast<std::int32_t>(group.rank() % 2);
            std::uint32_t wrapped = 0;
            for (std::uint32_t rank = 0; rank < n; ++rank)
                wrapped += static_cast<std::uint32_t>(
                    std::numeric_limits<std::int32_t>::max() - 1 +
                    static_cast<std::int32_t>(rank % 2));
            auto total = co_await group.all_reduce(mine, comm::ReduceOp::Sum);
            if (!total) co_return std::move(total).error();
            CHECK(static_cast<std::uint32_t>(*total) == wrapped);
            co_return {};
        });
    }

    TEST_CASE("ranks that name another element type fail on every rank") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() < 2) return;
        run_one([&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            dstd::Optional<dstd::Error> failure;
            if (group.rank() == 0) {
                auto total = co_await group.all_reduce(
                    static_cast<std::int32_t>(1), comm::ReduceOp::Sum);
                if (!total) failure = total.error();
            } else {
                auto total = co_await group.all_reduce(
                    static_cast<std::uint32_t>(1), comm::ReduceOp::Sum);
                if (!total) failure = total.error();
            }
            CHECK(failure.has_value());
            if (failure) CHECK(dftu_comm_test::has_text(*failure, "layout"));
            co_return {};
        });
    }
}
