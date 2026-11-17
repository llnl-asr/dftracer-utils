// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_COMM_FAULT_RUNS_H
#define DFTRACER_UTILS_TESTS_COMM_FAULT_RUNS_H

// A communicator over the fault provider, for the tests of faults and of
// observation: local bootstrap, the fault provider over local messaging, and
// the linear collectives over that provider, so a collective, a channel and a
// bulk transfer all pass through the rules.

#include <dftracer/utils/comm/backend/bootstrap.h>
#include <dftracer/utils/comm/backend/fault_messaging.h>
#include <dftracer/utils/comm/backend/linear_collectives.h>
#include <dftracer/utils/comm/backend/loader.h>
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/dstd/memory.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <utility>

namespace dftu_comm_test {

struct Faulty {
    dftracer::utils::comm::Communicator communicator;
    dftracer::utils::dstd::SharedPtr<
        dftracer::utils::comm::backend::FaultMessaging>
        fault;
};

inline Faulty start_faulty(std::uint32_t ranks,
                           dftracer::utils::coro::Executor& executor,
                           dftracer::utils::comm::backend::FaultConfig config) {
    auto bootstrap =
        dftracer::utils::comm::backend::make_local_bootstrap(ranks, executor);
    auto fault = dftracer::utils::comm::backend::make_fault_messaging(
        dftracer::utils::comm::backend::make_local_messaging(),
        std::move(config), bootstrap->timers());
    auto collectives =
        dftracer::utils::comm::backend::make_linear_collectives(fault);
    auto started = dftracer::utils::comm::Communicator::start(
        dftracer::utils::comm::Providers{std::move(bootstrap), fault,
                                         std::move(collectives)});
    REQUIRE(started.has_value());
    return Faulty{std::move(*started), std::move(fault)};
}

}  // namespace dftu_comm_test

#endif  // DFTRACER_UTILS_TESTS_COMM_FAULT_RUNS_H
