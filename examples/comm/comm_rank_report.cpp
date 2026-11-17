// SPDX-License-Identifier: MIT
// One program for every backend: each rank prints its place and the sum of all
// ranks. Run it in one process, or under a launcher after a build with
// -DDFTRACER_UTILS_ENABLE_MPI=ON.
//
// usage: comm_rank_report [local|mpi|auto]

#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>
#include <cstdio>
#include <utility>

using namespace dftracer::utils;

int main(int argc, char** argv) {
    // [start]
    comm::BackendKind kind = comm::BackendKind::Auto;
    if (argc > 1) {
        const dstd::StringView name = argv[1];
        if (name == "local") {
            kind = comm::BackendKind::Local;
        } else if (name == "mpi") {
            kind = comm::BackendKind::Mpi;
        } else if (name != "auto") {
            std::fprintf(stderr, "usage: %s [local|mpi|auto]\n", argv[0]);
            return 1;
        }
    }
    auto started = comm::Communicator::start(kind);
    if (!started) {
        std::fprintf(stderr, "%s\n", started.error().format().c_str());
        return 1;
    }
    comm::Communicator communicator = std::move(*started);
    comm::Group world = communicator.world(communicator.rank());
    // [start]

    // [sum]
    auto sum_op = world.all_reduce(static_cast<std::uint64_t>(world.rank()),
                                   comm::ReduceOp::Sum);
    auto sum = sum_op.wait();
    if (!sum) {
        std::fprintf(stderr, "%s\n", sum.error().format().c_str());
        return 1;
    }
    std::printf("rank %u of %u: sum of ranks %llu\n", world.rank(),
                world.size(), static_cast<unsigned long long>(*sum));
    // [sum]
    const std::uint64_t size = world.size();
    return *sum == size * (size - 1) / 2 ? 0 : 1;
}
