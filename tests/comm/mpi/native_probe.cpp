// SPDX-License-Identifier: MIT
// Ends a native collective that a peer never matches, in the ways a call
// ends early, and prints one line per rank with what that rank saw. Run under a
// launcher with at least two ranks. `abort`: rank 0 aborts its group while the
// others wait in an all_reduce. `deadline`: rank 0 never calls, and the others
// pass a deadline. The native requests that the peers left cannot be cancelled,
// so a rank that ends with one pending ends the job with MPI_Abort: the job
// must exit with a non-zero status, soon, after every rank printed its line.
#define DOCTEST_CONFIG_IMPLEMENT
#include <comm/support.h>
#include <dftracer/utils/comm/backend/messaging.h>
#include <dftracer/utils/comm/collective.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/runtime.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace dftracer::utils;

namespace {

using Task = coro::CoroTask<dstd::Result<void>>;

bool names(const dstd::Error& error, dstd::StringView text) {
    return dftu_comm_test::has_text(error, text);
}

void say(std::uint32_t rank, bool good, dstd::CStringView what) {
    std::printf("PROBE rank=%u %s %s\n", rank, good ? "ok" : "bad",
                what.c_str());
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    auto started = comm::Communicator::start(comm::BackendKind::Mpi);
    if (!started) {
        std::fprintf(stderr, "start failed: %s\n",
                     std::string(started.error().message()).c_str());
        return 3;
    }
    comm::Communicator communicator = std::move(*started);
    const bool abort_mode = argc > 1 && std::strcmp(argv[1], "abort") == 0;
    const bool create_mode = argc > 1 && std::strcmp(argv[1], "create") == 0;
    const std::uint32_t me = communicator.rank();
    if (create_mode) {
        if (communicator.size() < 2) return 4;
        auto messaging = comm::detail::Hooks::messaging(communicator);
        if (me == 0) {
            say(me, true, "stayed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            for (std::uint32_t peer = 1; peer < communicator.size(); ++peer)
                messaging->send(
                    comm::backend::MessageKey{9998, 0, peer, 5},
                    comm::backend::Payload(dstd::Vector<std::byte>(8)),
                    comm::backend::HOLD_EVERYTHING, {});
            std::this_thread::sleep_for(std::chrono::seconds(20));
            return 0;
        }
        auto collectives = comm::detail::Hooks::collectives(communicator);
        dstd::Vector<std::uint32_t> members;
        for (std::uint32_t rank = 0; rank < communicator.size(); ++rank)
            members.push_back(rank);
        const std::uint64_t begin = dstd::steady_now_ns();
        collectives->bind_group(9999, dstd::Span<const std::uint32_t>(
                                          members.data(), members.size()));
        comm::backend::Record record;
        record.kind =
            static_cast<std::uint32_t>(comm::detail::CallKind::Barrier);
        collectives->arrive(9999, 0, communicator.size(), me, record, {},
                            [](const comm::backend::SlotEnd&) {});
        std::atomic<bool> got{false};
        messaging->recv(comm::backend::MessageKey{9998, 0, me, 5},
                        [&got](dstd::Span<const std::byte>,
                               const dstd::Error*) { got.store(true); });
        const bool fast = dstd::steady_now_ns() - begin < 1000000000ull;
        const bool delivered = dftu_comm_test::spin_until(
            [&] { return got.load(); }, std::chrono::seconds(8));
        say(me, fast && delivered,
            "delivered a message with a creation pending");
        return 0;
    }
    comm::Group group = communicator.world(me);
    if (communicator.size() < 2) return 4;
    if (me == 0) {
        if (abort_mode) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            group.abort(
                dstd::Error(dstd::Condition::Internal, "rank zero gave up"));
            say(me, true, "aborted");
        } else {
            say(me, true, "stayed out");
        }
        std::this_thread::sleep_for(std::chrono::seconds(20));
        return 0;
    }
    Runtime rt(2);
    auto errors = dftu_comm_test::run_ranks(rt, 1, [&](std::uint32_t) -> Task {
        comm::CallOpts opts;
        if (!abort_mode)
            opts.deadline_ns = dftu_comm_test::deadline_after(400'000'000);
        auto total = co_await group.all_reduce(static_cast<std::int32_t>(1),
                                               comm::ReduceOp::Sum, opts);
        if (total) {
            say(me, false, "the call succeeded");
        } else if (abort_mode) {
            say(me,
                names(total.error(), "aborted by rank 0") &&
                    names(total.error(), "rank zero gave up"),
                "failed naming rank 0");
        } else {
            say(me,
                names(total.error(), "deadline") &&
                    names(total.error(), "missing ranks: 0"),
                "failed naming the missing rank 0");
        }
        co_return {};
    });
    return errors[0] ? 5 : 0;
}
