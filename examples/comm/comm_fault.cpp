// SPDX-License-Identifier: MIT
// Fault injection under a communicator: a seeded delay on every message, then
// a rank that dies while its peers wait for it.
//
// usage: comm_fault

#include <dftracer/utils/comm/backend/bootstrap.h>
#include <dftracer/utils/comm/backend/fault_messaging.h>
#include <dftracer/utils/comm/backend/linear_collectives.h>
#include <dftracer/utils/comm/backend/loader.h>
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/runtime/runtime.h>

#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>

using namespace dftracer::utils;

namespace {

constexpr std::uint32_t RANKS = 3;
constexpr std::uint64_t LONG_NS = 20'000'000'000ull;

struct Faulty {
    comm::Communicator communicator;
    dstd::SharedPtr<comm::backend::FaultMessaging> fault;
};

// [start]
// The fault provider sits over the local messaging, and the collectives run
// over the fault provider, so a barrier, a channel and a bulk transfer all
// pass through the rules.
dstd::Result<Faulty> start_faulty(Runtime& rt,
                                  comm::backend::FaultConfig config) {
    auto bootstrap = comm::backend::make_local_bootstrap(RANKS, *rt.executor());
    auto fault = comm::backend::make_fault_messaging(
        comm::backend::make_local_messaging(), std::move(config),
        bootstrap->timers());
    auto collectives = comm::backend::make_linear_collectives(fault);
    auto started = comm::Communicator::start(
        comm::Providers{std::move(bootstrap), fault, std::move(collectives)});
    if (!started) return dstd::Unexpected(started.error());
    return Faulty{std::move(*started), std::move(fault)};
}
// [start]

comm::CallOpts within(std::uint64_t ns) {
    comm::CallOpts opts;
    opts.deadline_ns = dstd::steady_now_ns() + ns;
    return opts;
}

bool delayed_barrier(Runtime& rt) {
    // [delay]
    comm::backend::FaultConfig config;
    config.seed = 7;
    comm::backend::FaultRule slow;
    slow.delay_min_ns = 100'000;
    slow.delay_max_ns = 2'000'000;
    config.rules.push_back(slow);

    auto run = start_faulty(rt, std::move(config));
    if (!run) return false;
    dstd::Vector<comm::Op<void>> calls;
    for (std::uint32_t rank = 0; rank < RANKS; ++rank)
        calls.push_back(run->communicator.world(rank).barrier(within(LONG_NS)));
    bool ok = true;
    for (comm::Op<void>& call : calls) ok = call.wait().has_value() && ok;
    // The same seed gives the same delays on every run.
    std::printf("delayed %llu messages, barrier %s\n",
                static_cast<unsigned long long>(run->fault->counts().delayed),
                ok ? "ok" : "failed");
    // [delay]
    return ok && run->fault->counts().delayed > 0;
}

bool killed_rank(Runtime& rt) {
    // [kill]
    auto run = start_faulty(rt, comm::backend::FaultConfig{});
    if (!run) return false;
    comm::Op<void> zero = run->communicator.world(0).barrier(within(LONG_NS));
    comm::Op<void> one = run->communicator.world(1).barrier(within(LONG_NS));
    // Rank 2 never calls the barrier. Wait until both calls are registered,
    // then kill it.
    while (run->communicator.pending().size() < 2) std::this_thread::yield();
    run->fault->kill(2);
    auto first = zero.wait();
    auto second = one.wait();
    // Every pending call of the group ends with the same error, which names the
    // rank.
    if (first || second) return false;
    std::printf("killed: %s\n", first.error().format().c_str());
    // [kill]
    return first.error().message() == second.error().message();
}

}  // namespace

int main() {
    Runtime rt(4);
    const bool delayed = delayed_barrier(rt);
    const bool killed = killed_rank(rt);
    return delayed && killed ? 0 : 1;
}
