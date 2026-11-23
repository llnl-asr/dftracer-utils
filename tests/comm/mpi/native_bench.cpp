// SPDX-License-Identifier: MIT
// Times four group calls on the MPI backend, as one process per rank under a
// launcher: barrier, all_reduce of 8 bytes, all_gather of 1 KiB and broadcast
// of 64 KiB. Prints the median microseconds per call over the rounds, on rank
// 0. Run it once as is and once with DFTU_TEST_LINEAR=1 to
// compare the native collectives with the ones over the messages.
#define DOCTEST_CONFIG_IMPLEMENT
#include <comm/support.h>
#include <dftracer/utils/comm/backend/linear_collectives.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace dftracer::utils;

namespace {

using Task = coro::CoroTask<dstd::Result<void>>;

constexpr int ROUNDS = 7;

struct Row {
    const char* name;
    int calls;
};

}  // namespace

int main(int argc, char** argv) {
    const int scale = argc > 1 ? std::atoi(argv[1]) : 1;
    auto started = comm::Communicator::start(comm::BackendKind::Mpi);
    if (!started) return 3;
    comm::Communicator communicator = std::move(*started);
    if (common::Env::get("DFTU_TEST_LINEAR").has_value())
        comm::detail::Hooks::set_collectives(
            communicator, comm::backend::make_linear_collectives(
                              comm::detail::Hooks::messaging(communicator)));
    comm::Group group = communicator.world(communicator.rank());
    const Row rows[4] = {{"barrier", 400 * scale},
                         {"all_reduce 8 B int", 400 * scale},
                         {"all_gather 1 KiB", 200 * scale},
                         {"broadcast 64 KiB", 200 * scale}};
    const dstd::Vector<std::byte> kib(1024, std::byte{1});
    const dstd::Vector<std::byte> block(64 * 1024, std::byte{2});
    Runtime rt(2);
    for (int row = 0; row < 4; ++row) {
        dstd::Vector<double> micros;
        for (int round = 0; round < ROUNDS; ++round) {
            double elapsed = 0;
            auto errors =
                dftu_comm_test::run_ranks(rt, 1, [&](std::uint32_t) -> Task {
                    auto warm = co_await group.barrier();
                    if (!warm) co_return warm;
                    const std::uint64_t begin = dstd::steady_now_ns();
                    for (int call = 0; call < rows[row].calls; ++call) {
                        if (row == 0) {
                            auto done = co_await group.barrier();
                            if (!done) co_return done;
                        } else if (row == 1) {
                            auto total = co_await group.all_reduce(
                                static_cast<std::int64_t>(1),
                                comm::ReduceOp::Sum);
                            if (!total) co_return std::move(total).error();
                        } else if (row == 2) {
                            auto all = co_await group.all_gather(
                                dstd::Span<const std::byte>(kib.data(),
                                                            kib.size()));
                            if (!all) co_return std::move(all).error();
                        } else {
                            auto value = co_await group.broadcast(
                                0, group.rank() == 0
                                       ? dstd::Span<const std::byte>(
                                             block.data(), block.size())
                                       : dstd::Span<const std::byte>());
                            if (!value) co_return std::move(value).error();
                        }
                    }
                    elapsed =
                        static_cast<double>(dstd::steady_now_ns() - begin);
                    co_return {};
                });
            if (errors[0]) return 4;
            micros.push_back(elapsed / 1000.0 / rows[row].calls);
        }
        std::sort(micros.begin(), micros.end());
        if (communicator.rank() == 0)
            std::printf("%-20s %10.1f us per call\n", rows[row].name,
                        micros[micros.size() / 2]);
    }
    return 0;
}
