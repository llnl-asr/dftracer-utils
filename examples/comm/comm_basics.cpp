// SPDX-License-Identifier: MIT
// Three ranks of one communicator in one process: groups, collectives,
// channels, an object call, bulk transfers, a deadline and the counters.
//
// usage: comm_basics

#include <dftracer/utils/comm/channel.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>

using namespace dftracer::utils;

namespace {

constexpr std::uint32_t RANKS = 3;

dstd::Error wrong(dstd::StringView what) {
    return dstd::Error{dstd::Condition::Internal, what};
}

// [run_ranks]
// Runs body(rank) for every rank at once and says whether all of them
// succeeded. A rank is a coroutine, so the ranks of one run share the
// executor of the runtime.
template <class Body>
bool run_ranks(Runtime& rt, dstd::CStringView name, Body body) {
    dstd::Vector<dstd::Optional<dstd::Error>> errors(RANKS);
    auto driver = coro::with_scope(
        [&body, &errors](coro::Scope& scope) -> coro::CoroTask<void> {
            for (std::uint32_t rank = 0; rank < RANKS; ++rank) {
                scope.spawn([&body, &errors, rank]() -> coro::CoroTask<void> {
                    auto outcome = co_await body(rank);
                    if (!outcome) errors[rank] = std::move(outcome).error();
                });
            }
            co_return;
        });
    auto result = rt.block_on(std::move(driver));
    bool ok = result.has_value();
    for (std::uint32_t rank = 0; rank < RANKS; ++rank) {
        if (!errors[rank]) continue;
        std::printf("%s: rank %u failed: %s\n", name.c_str(), rank,
                    errors[rank]->format().c_str());
        ok = false;
    }
    if (ok) std::printf("%s: ok\n", name.c_str());
    return ok;
}
// [run_ranks]

// [groups]
coro::CoroTask<dstd::Result<void>> groups(comm::Communicator& communicator,
                                          std::uint32_t rank) {
    comm::Group world = communicator.world(rank);
    comm::Group node = communicator.local(rank);
    if (world.size() != RANKS || node.size() != RANKS)
        co_return wrong("one process holds every rank");

    // Ranks 0 and 2 take color 0 and rank 1 takes color 1. The key orders the
    // ranks of a new group.
    auto split_op = world.split(rank % 2, rank);
    auto split = co_await split_op;
    if (!split) co_return std::move(split).error();
    comm::Group half = *split;
    if (half.size() != (rank % 2 == 0 ? 2u : 1u))
        co_return wrong("split made a group of the wrong size");

    auto barrier_op = half.barrier();
    auto ended = co_await barrier_op;
    co_return ended;
}
// [groups]

// [collectives]
coro::CoroTask<dstd::Result<void>> collectives(comm::Communicator& communicator,
                                               std::uint32_t rank) {
    comm::Group world = communicator.world(rank);

    auto broadcast_op = world.broadcast(0, std::uint64_t{rank == 0 ? 42u : 0u});
    auto broadcast = co_await broadcast_op;
    if (!broadcast) co_return std::move(broadcast).error();
    if (*broadcast != 42) co_return wrong("broadcast gave another value");

    const std::uint64_t mine = rank * 10;
    auto gather_op = world.all_gather<std::uint64_t>(
        dstd::Span<const std::uint64_t>(&mine, 1));
    auto gathered = co_await gather_op;
    if (!gathered) co_return std::move(gathered).error();
    for (std::uint32_t peer = 0; peer < RANKS; ++peer)
        if (gathered->as<std::uint64_t>(peer)[0] != peer * 10)
            co_return wrong("all_gather gave another value");

    // The merge folds the ranks in rank order, so a double sums to the same
    // bits on every rank and on every run.
    auto reduce_op = world.all_reduce(0.1 * (rank + 1), comm::ReduceOp::Sum);
    auto sum = co_await reduce_op;
    if (!sum) co_return std::move(sum).error();
    if (rank == 0) std::printf("all_reduce sum: %.17g\n", *sum);

    // Every rank holds the same bits, and they are those of the fold in rank
    // order.
    double folded = 0.1 * 1;
    folded += 0.1 * 2;
    folded += 0.1 * 3;
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(*sum);
    if (bits != std::bit_cast<std::uint64_t>(folded))
        co_return wrong("the sum is not the fold in rank order");
    auto bits_op = world.all_gather<std::uint64_t>(
        dstd::Span<const std::uint64_t>(&bits, 1));
    auto all_bits = co_await bits_op;
    if (!all_bits) co_return std::move(all_bits).error();
    for (std::uint32_t peer = 0; peer < RANKS; ++peer)
        if (all_bits->as<std::uint64_t>(peer)[0] != bits)
            co_return wrong("the ranks hold different sums");
    co_return {};
}
// [collectives]

// [channels]
coro::CoroTask<dstd::Result<void>> channels(comm::Communicator& communicator,
                                            std::uint32_t rank) {
    comm::Group world = communicator.world(rank);

    // A stream of depth 2: a third send waits until a receive frees a slot.
    comm::Channel<std::uint64_t> tokens = world.channel<std::uint64_t>(
        "tokens", comm::ChannelOpts{2, comm::Overflow::Block});
    // A stream of depth 1 that fails a send at once when it is full.
    comm::Channel<std::uint64_t> alerts = world.channel<std::uint64_t>(
        "alerts", comm::ChannelOpts{1, comm::Overflow::Fail});

    if (rank == 0) {
        for (std::uint64_t token = 1; token <= 4; ++token) {
            auto send_op = tokens.send(1, token);
            auto sent = co_await send_op;
            if (!sent) co_return sent;
        }
        tokens.close();

        auto first_op = alerts.send(2, 7);
        auto first = co_await first_op;
        if (!first) co_return first;
        auto second_op = alerts.send(2, 8);
        auto second = co_await second_op;
        if (second || !second.error().is_code(comm::ChannelError::Full))
            co_return wrong("a full stream must refuse the send");
    }

    if (rank == 1) {
        std::uint64_t total = 0;
        bool closed = false;
        while (!closed) {
            auto recv_op = tokens.recv_from(0);
            auto message = co_await recv_op;
            if (message) {
                total += message->value();
            } else if (message.error().is_code(comm::ChannelError::Closed)) {
                closed = true;
            } else {
                co_return std::move(message).error();
            }
        }
        if (total != 10) co_return wrong("the tokens did not add up");
    }

    // Rank 2 reads its alert only after rank 0 saw the stream refuse the
    // second one.
    auto barrier_op = world.barrier();
    auto reached = co_await barrier_op;
    if (!reached) co_return reached;

    if (rank == 2) {
        auto recv_op = alerts.recv_from(0);
        auto message = co_await recv_op;
        if (!message) co_return std::move(message).error();
        if (message->value() != 7) co_return wrong("the first alert is kept");
    }
    co_return {};
}
// [channels]

// [object]
coro::CoroTask<dstd::Result<void>> objects(comm::Communicator& communicator,
                                           std::uint32_t rank) {
    comm::Group world = communicator.world(rank);
    dstd::String name = "rank ";
    name.push_back(static_cast<char>('0' + rank));

    // A string is not trivially copyable. All three ranks share this process,
    // so each one reads the object of its peer with no copy and no codec call.
    auto gather_op =
        world.all_gather_object(std::move(name), comm::StringCodec{});
    auto gathered = co_await gather_op;
    if (!gathered) co_return std::move(gathered).error();
    if (gathered->size() != RANKS || *(*gathered)[2] != "rank 2")
        co_return wrong("all_gather_object gave another value");
    co_return {};
}
// [object]

// [bulk]
coro::CoroTask<dstd::Result<void>> bulk(comm::Communicator& communicator,
                                        std::uint32_t rank) {
    comm::Group world = communicator.world(rank);
    comm::Channel<comm::BufferRef> refs =
        world.channel<comm::BufferRef>("refs");

    dstd::Vector<std::byte> memory(16, std::byte{1});
    if (rank == 0) {
        // Expose is not collective. The descriptor holds no pointer, so it
        // travels over a channel.
        auto ref =
            world.expose(dstd::Span<std::byte>(memory.data(), memory.size()),
                         comm::Access::ReadWrite);
        if (!ref) co_return std::move(ref).error();
        auto send_op = refs.send(1, *ref);
        auto sent = co_await send_op;
        if (!sent) co_return sent;

        auto barrier_op = world.barrier();
        auto reached = co_await barrier_op;
        if (!reached) co_return reached;

        // The memory stays valid until the Op of unexpose ends.
        auto unexpose_op = world.unexpose(*ref);
        auto closed = co_await unexpose_op;
        if (!closed) co_return closed;
        if (memory[4] != std::byte{9}) co_return wrong("the push did not land");
        co_return {};
    }

    if (rank == 1) {
        auto recv_op = refs.recv_from(0);
        auto message = co_await recv_op;
        if (!message) co_return std::move(message).error();
        const comm::BufferRef ref = message->value();

        dstd::Vector<std::byte> copy(16);
        auto pull_op =
            world.pull(ref, dstd::Span<std::byte>(copy.data(), copy.size()));
        auto pulled = co_await pull_op;
        if (!pulled) co_return std::move(pulled).error();
        if (*pulled != 16 || copy[15] != std::byte{1})
            co_return wrong("the pull copied the wrong bytes");

        const dstd::Vector<std::byte> patch(4, std::byte{9});
        auto push_op = world.push(
            ref, dstd::Span<const std::byte>(patch.data(), patch.size()), 4);
        auto pushed = co_await push_op;
        if (!pushed) co_return std::move(pushed).error();
    }

    auto barrier_op = world.barrier();
    auto reached = co_await barrier_op;
    co_return reached;
}
// [bulk]

// [deadline]
// A deadline that passes on one rank aborts the group on every rank, so this
// runs on a communicator of its own.
bool deadline_demo(Runtime& rt) {
    auto started = comm::Communicator::start_local(RANKS, *rt.executor());
    if (!started) return false;
    comm::Communicator communicator = std::move(*started);
    dstd::Optional<dstd::Error> seen;

    comm::CallOpts opts;
    opts.deadline_ns = dstd::steady_now_ns() + 100'000'000;
    auto run = [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
        // Rank 2 never calls the barrier, and the other two wait for it.
        if (rank == 2) co_return {};
        comm::Group world = communicator.world(rank);
        auto barrier_op = world.monitored_barrier(opts);
        auto ended = co_await barrier_op;
        if (ended) co_return wrong("a missing rank must end the barrier");
        if (rank == 0) seen = ended.error();
        co_return {};
    };
    const bool ok = run_ranks(rt, "deadline", run);
    if (seen) std::printf("deadline error: %s\n", seen->format().c_str());
    // The error names the rank that never arrived.
    const bool names_rank = seen && seen->message().find("missing ranks: 2") !=
                                        dstd::StringView::npos;
    return ok && names_rank;
}
// [deadline]

}  // namespace

int main() {
    Runtime rt(4);
    // [start]
    // Every rank is a coroutine of this process, on the executor of the
    // runtime, which must outlive the communicator.
    auto started = comm::Communicator::start_local(RANKS, *rt.executor());
    if (!started) {
        std::fprintf(stderr, "%s\n", started.error().format().c_str());
        return 1;
    }
    comm::Communicator communicator = std::move(*started);
    // [start]

    bool ok = true;
    ok = run_ranks(rt, "groups",
                   [&communicator](std::uint32_t rank) {
                       return groups(communicator, rank);
                   }) &&
         ok;
    ok = run_ranks(rt, "collectives",
                   [&communicator](std::uint32_t rank) {
                       return collectives(communicator, rank);
                   }) &&
         ok;
    ok = run_ranks(rt, "channels",
                   [&communicator](std::uint32_t rank) {
                       return channels(communicator, rank);
                   }) &&
         ok;
    ok = run_ranks(rt, "objects",
                   [&communicator](std::uint32_t rank) {
                       return objects(communicator, rank);
                   }) &&
         ok;
    ok = run_ranks(rt, "bulk",
                   [&communicator](std::uint32_t rank) {
                       return bulk(communicator, rank);
                   }) &&
         ok;

    // [stats]
    for (const comm::CallStats& row : communicator.stats())
        std::printf("group %llu %.*s: started %llu ok %llu failed %llu\n",
                    static_cast<unsigned long long>(row.group),
                    static_cast<int>(row.kind.size()), row.kind.data(),
                    static_cast<unsigned long long>(row.started),
                    static_cast<unsigned long long>(row.ok),
                    static_cast<unsigned long long>(row.failed));
    std::printf("pending calls: %zu\n", communicator.pending().size());
    // [stats]
    if (!communicator.pending().empty()) ok = false;

    ok = deadline_demo(rt) && ok;
    return ok ? 0 : 1;
}
