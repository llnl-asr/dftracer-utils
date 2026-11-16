// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BACKEND_BOOTSTRAP_H
#define DFTRACER_UTILS_COMM_BACKEND_BOOTSTRAP_H

#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::coro {
class TimerService;
}  // namespace dftracer::utils::coro

namespace dftracer::utils::comm::backend {

/**
 * @brief What a group learns from the backend that started the run: how many
 * ranks it has, which of them share a node, and where it measures.
 *
 * One provider serves a whole communicator and outlives its groups.
 */
class Bootstrap {
   public:
    Bootstrap() = default;
    Bootstrap(const Bootstrap&) = delete;
    Bootstrap& operator=(const Bootstrap&) = delete;
    virtual ~Bootstrap() = default;

    /// How many ranks the run has.
    virtual std::uint32_t ranks() const noexcept = 0;

    /// The world ranks that share a node with this rank, in node order, so the
    /// node rank is the place of a rank in the result.
    virtual dstd::Vector<std::uint32_t> node_members() const = 0;

    /// Where a group measures its deadlines. Must outlive the communicator.
    virtual coro::TimerService* timers() const noexcept = 0;
};

}  // namespace dftracer::utils::comm::backend

#endif  // DFTRACER_UTILS_COMM_BACKEND_BOOTSTRAP_H
