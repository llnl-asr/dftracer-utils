// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/support.h>
#include <dftracer/utils/comm/backend/bulk.h>
#include <dftracer/utils/comm/bulk.h>
#include <dftracer/utils/comm/channel.h>
#include <dftracer/utils/comm/error.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

// Runs as one process of an mpirun job, so each rank is a process and every
// pull and push crosses the MPI backend loaded at run time.

using namespace dftracer::utils;

namespace {

using Bytes = dstd::Vector<std::byte>;
using Task = coro::CoroTask<dstd::Result<void>>;

comm::Communicator start_mpi() {
    auto started = comm::Communicator::start(comm::BackendKind::Mpi);
    if (!started) FAIL(std::string(started.error().c_str()));
    return std::move(*started);
}

template <class Body>
dstd::Optional<dstd::Error> run_here(Runtime& rt, Body body) {
    auto errors = dftu_comm_test::run_ranks(
        rt, 1, [&body](std::uint32_t) { return body(); });
    return errors[0];
}

Bytes pattern(std::size_t size, std::uint32_t seed) {
    Bytes bytes(size);
    for (std::size_t at = 0; at < size; ++at)
        bytes[at] = static_cast<std::byte>((at * 31 + seed * 7 + 1) & 0xFF);
    return bytes;
}

dstd::Span<std::byte> view(Bytes& bytes) {
    return dstd::Span<std::byte>(bytes.data(), bytes.size());
}

dstd::Span<const std::byte> cview(const Bytes& bytes) {
    return dstd::Span<const std::byte>(bytes.data(), bytes.size());
}

bool same(dstd::Span<const std::byte> a, dstd::Span<const std::byte> b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

/// Every rank's ref, gathered, in rank order.
dstd::Result<dstd::Vector<comm::BufferRef>> gather_refs(
    comm::Group& group, const comm::BufferRef& mine) {
    auto gathered_op =
        group.all_gather(dstd::Span<const comm::BufferRef>(&mine, 1));
    auto gathered = gathered_op.wait();
    if (!gathered) return dstd::Unexpected(gathered.error());
    dstd::Vector<comm::BufferRef> refs(group.size());
    for (std::uint32_t rank = 0; rank < group.size(); ++rank)
        std::memcpy(&refs[rank], (*gathered)[rank].data(),
                    sizeof(comm::BufferRef));
    return refs;
}

}  // namespace

TEST_SUITE("CommMpiBulk") {
    TEST_CASE("a pull and a push move bytes of every size between processes") {
        // 4 KiB MPI chunks, 32 KiB ranges and 64 KiB credit, so the larger
        // sizes run in many ranges, each cut in several MPI messages.
        ::setenv("DFTRACER_UTILS_COMM_MPI_CHUNK_BYTES", "4096", 1);
        comm::Communicator communicator = start_mpi();
        ::unsetenv("DFTRACER_UTILS_COMM_MPI_CHUNK_BYTES");
        auto bulk = comm::detail::Hooks::bulk(communicator);
        bulk->limit_range_bytes(32 << 10);
        bulk->limit_credit_bytes(64 << 10);
        Runtime rt(2);
        auto messaging = comm::detail::Hooks::messaging(communicator);
        for (const std::size_t size :
             {std::size_t{100}, std::size_t{65536}, std::size_t{65537},
              std::size_t{300000}}) {
            INFO(size);
            const std::uint64_t landed_before = messaging->landed_bytes();
            auto error = run_here(rt, [&]() -> Task {
                comm::Group group = communicator.world(communicator.rank());
                const std::uint32_t n = group.size();
                const std::uint32_t me = group.rank();
                const std::uint32_t next = (me + 1) % n;
                const std::uint32_t before = (me + n - 1) % n;
                Bytes memory = pattern(size, me);
                auto ref = group.expose(view(memory));
                if (!ref) co_return std::move(ref).error();
                auto refs = gather_refs(group, *ref);
                if (!refs) co_return std::move(refs).error();

                Bytes got(size);
                auto pulled = co_await group.pull((*refs)[next], view(got));
                if (!pulled) co_return std::move(pulled).error();
                CHECK(*pulled == size);
                CHECK(same(cview(got), cview(pattern(size, next))));
                auto joined = co_await group.barrier();
                if (!joined) co_return joined;

                const Bytes fresh = pattern(size / 2, 100 + me);
                auto pushed =
                    co_await group.push((*refs)[next], cview(fresh), size / 4);
                if (!pushed) co_return std::move(pushed).error();
                CHECK(*pushed == fresh.size());
                joined = co_await group.barrier();
                if (!joined) co_return joined;
                const Bytes wrote = pattern(size / 2, 100 + before);
                CHECK(same(dstd::Span<const std::byte>(memory.data() + size / 4,
                                                       wrote.size()),
                           cview(wrote)));
                joined = co_await group.barrier();
                if (!joined) co_return joined;
                auto closed = co_await group.unexpose(*ref);
                co_return closed;
            });
            CHECK_FALSE(error.has_value());
            // The ranges of a large pull and of a large push went from the wire
            // into the buffer, the caller's and the exposure, with no copy
            // through a message: this rank pulled the whole size and was
            // pushed half of it. Eager transfers may arrive before their
            // receive, so only the large size says so.
            if (communicator.size() > 1 && size == 300000)
                CHECK(messaging->landed_bytes() - landed_before ==
                      size + size / 2);
        }
        // No peer ever had more than its credit in flight toward a receiver.
        CHECK(bulk->peak_in_flight() > 0);
        CHECK(bulk->peak_in_flight() <= (64 << 10));
    }

    TEST_CASE("the owner and the caller refuse what the ref does not allow") {
        comm::Communicator communicator = start_mpi();
        auto bulk = comm::detail::Hooks::bulk(communicator);
        bulk->limit_range_bytes(32 << 10);
        Runtime rt(2);
        auto error = run_here(rt, [&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            const std::uint32_t n = group.size();
            const std::uint32_t me = group.rank();
            const std::uint32_t next = (me + 1) % n;
            constexpr std::size_t SIZE = 200000;
            Bytes read_memory = pattern(SIZE, me);
            Bytes write_memory(SIZE);
            auto read_ref =
                group.expose(view(read_memory), comm::Access::ReadOnly);
            auto write_ref =
                group.expose(view(write_memory), comm::Access::WriteOnly);
            if (!read_ref) co_return std::move(read_ref).error();
            if (!write_ref) co_return std::move(write_ref).error();
            auto reads = gather_refs(group, *read_ref);
            auto writes = gather_refs(group, *write_ref);
            if (!reads) co_return std::move(reads).error();
            if (!writes) co_return std::move(writes).error();
            const comm::BufferRef readable = (*reads)[next];
            const comm::BufferRef writable = (*writes)[next];
            Bytes got(SIZE);
            const Bytes fresh = pattern(64, 5);

            // Refused by the caller, from the descriptor.
            auto refused_push = co_await group.push(readable, cview(fresh));
            CHECK_FALSE(refused_push.has_value());
            if (!refused_push) {
                CHECK(
                    refused_push.error().is(dstd::Condition::InvalidArgument));
                CHECK(dftu_comm_test::has_text(refused_push.error(),
                                               "read only"));
            }
            auto refused_pull = co_await group.pull(writable, view(got));
            CHECK_FALSE(refused_pull.has_value());
            if (!refused_pull)
                CHECK(dftu_comm_test::has_text(refused_pull.error(),
                                               "write only"));

            // TooBig from the caller's checks, with nothing moved.
            Bytes short_buffer(SIZE - 1, std::byte{0x42});
            auto too_short = co_await group.pull(readable, view(short_buffer));
            CHECK_FALSE(too_short.has_value());
            if (!too_short)
                CHECK(too_short.error().is_code(comm::ChannelError::TooBig));
            CHECK(short_buffer[0] == std::byte{0x42});
            const Bytes too_long = pattern(SIZE + 1, 1);
            auto long_push = co_await group.push(writable, cview(too_long));
            CHECK_FALSE(long_push.has_value());
            if (!long_push)
                CHECK(long_push.error().is_code(comm::ChannelError::TooBig));

            // A descriptor that lies: the owner checks against its own record.
            comm::BufferRef lying = readable;
            lying.access = comm::Access::ReadWrite;
            auto owner_refuses = co_await group.push(lying, cview(fresh));
            CHECK_FALSE(owner_refuses.has_value());
            if (!owner_refuses)
                CHECK(dftu_comm_test::has_text(owner_refuses.error(),
                                               "read only"));
            lying = readable;
            lying.size = SIZE + 100;
            Bytes bigger(SIZE + 100);
            auto owner_too_big = co_await group.pull(lying, view(bigger));
            CHECK_FALSE(owner_too_big.has_value());
            if (!owner_too_big)
                CHECK(
                    owner_too_big.error().is_code(comm::ChannelError::TooBig));
            lying = writable;
            lying.size = SIZE + 100;
            const Bytes long_eager = pattern(SIZE + 100, 2);
            auto owner_long_push =
                co_await group.push(lying, cview(long_eager));
            CHECK_FALSE(owner_long_push.has_value());
            if (!owner_long_push)
                CHECK(owner_long_push.error().is_code(
                    comm::ChannelError::TooBig));
            const Bytes long_small = pattern(100, 3);
            auto past_end =
                co_await group.push(lying, cview(long_small), SIZE - 10);
            CHECK_FALSE(past_end.has_value());
            if (!past_end)
                CHECK(past_end.error().is_code(comm::ChannelError::TooBig));

            comm::BufferRef unknown = readable;
            unknown.id = 987654;
            auto missing = co_await group.pull(unknown, view(got));
            CHECK_FALSE(missing.has_value());
            if (!missing) {
                CHECK(missing.error().is(dstd::Condition::NotFound));
                CHECK(dftu_comm_test::has_text(missing.error(), "987654"));
            }

            auto joined = co_await group.barrier();
            if (!joined) co_return joined;
            auto closed = co_await group.unexpose(*read_ref);
            if (!closed) co_return closed;
            auto closed_writes = co_await group.unexpose(*write_ref);
            if (!closed_writes) co_return closed_writes;
            co_return {};
        });
        CHECK_FALSE(error.has_value());
    }

    TEST_CASE("an unexpose ends pulls from other processes cleanly") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() < 2) return;
        auto bulk = comm::detail::Hooks::bulk(communicator);
        bulk->limit_range_bytes(16 << 10);
        bulk->limit_credit_bytes(32 << 10);
        Runtime rt(2);
        auto error = run_here(rt, [&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            constexpr std::size_t SIZE = std::size_t{8} << 20;
            const Bytes expected = pattern(SIZE, 0);
            Bytes memory = expected;
            comm::BufferRef mine{};
            if (group.rank() == 0) {
                auto ref = group.expose(view(memory), comm::Access::ReadOnly);
                if (!ref) co_return std::move(ref).error();
                mine = *ref;
            }
            auto refs = gather_refs(group, mine);
            if (!refs) co_return std::move(refs).error();
            auto started = group.channel<std::uint32_t>("started");
            if (group.rank() == 0) {
                // Closing while the peers pull: each peer says that its pull
                // took credit, which it takes only once it is under way.
                for (std::uint32_t peer = 1; peer < group.size(); ++peer) {
                    auto said = co_await started.recv_from(peer);
                    if (!said) co_return std::move(said).error();
                }
                auto closed = co_await group.unexpose(mine);
                if (!closed) co_return closed;
                std::memset(memory.data(), 0xEE, memory.size());
            } else {
                Bytes got(SIZE);
                auto pull = group.pull((*refs)[0], view(got));
                if (!dftu_comm_test::spin_until(
                        [&] { return bulk->peak_in_flight() > 0; }))
                    co_return dstd::Error(dstd::Condition::Timeout,
                                          "the pull never started");
                auto told = co_await started.send(0, 1u);
                if (!told) co_return told;
                auto pulled = co_await pull;
                if (pulled) {
                    CHECK(*pulled == SIZE);
                    CHECK(same(cview(got), cview(expected)));
                } else {
                    CHECK(pulled.error().is(dstd::Condition::NotFound));
                }
            }
            auto joined = co_await group.barrier();
            co_return joined;
        });
        CHECK_FALSE(error.has_value());
    }

    TEST_CASE("an abort during a pull fails it with the error of the group") {
        comm::Communicator communicator = start_mpi();
        if (communicator.size() < 2) return;
        auto bulk = comm::detail::Hooks::bulk(communicator);
        bulk->limit_range_bytes(4096);
        bulk->limit_credit_bytes(8192);
        Runtime rt(2);
        auto error = run_here(rt, [&]() -> Task {
            comm::Group group = communicator.world(communicator.rank());
            constexpr std::size_t SIZE = std::size_t{64} << 20;
            Bytes memory;
            comm::BufferRef mine{};
            if (group.rank() == 1) {
                memory = pattern(SIZE, 1);
                auto ref = group.expose(view(memory), comm::Access::ReadOnly);
                if (!ref) co_return std::move(ref).error();
                mine = *ref;
            }
            auto refs = gather_refs(group, mine);
            if (!refs) co_return std::move(refs).error();
            auto parked = group.channel<std::uint32_t>("parked");
            if (group.rank() == 1) {
                // Rank 0 says that its pull waits, and this rank aborts then.
                auto said = co_await parked.recv_from(0);
                if (!said) co_return std::move(said).error();
                group.abort(dstd::Error(dstd::Condition::Internal,
                                        "the owner gave up"));
                co_return {};
            }
            if (group.rank() == 0) {
                // The credit toward rank 1 is taken, so the pull cannot move
                // a byte before the abort.
                auto held = bulk->hold_credit(group.id(), 0, 1, 8192);
                Bytes got(SIZE);
                auto pull = group.pull((*refs)[1], view(got));
                CHECK_FALSE(pull.test());
                auto told = co_await parked.send(1, 1u);
                if (!told) co_return told;
                auto pulled = co_await pull;
                CHECK_FALSE(pulled.has_value());
                if (!pulled) {
                    CHECK(dftu_comm_test::has_text(pulled.error(),
                                                   "aborted by rank 1"));
                    CHECK(dftu_comm_test::has_text(pulled.error(),
                                                   "the owner gave up"));
                }
            }
            co_return {};
        });
        CHECK_FALSE(error.has_value());
    }
}
