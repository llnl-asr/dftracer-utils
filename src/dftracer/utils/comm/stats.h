// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_STATS_H
#define DFTRACER_UTILS_COMM_STATS_H

#include <dftracer/utils/dstd/string.h>

#include <cstdint>

namespace dftracer::utils::comm {

/**
 * @brief What the calls of one kind made on one group ended with, at one
 * moment.
 *
 * A call counts once for each rank that makes it. @ref kind names the call as
 * its method does, `barrier` or `all_reduce`, or `channel_send`,
 * `channel_recv`, `pull` or `push`. The bytes are those of the calls that ended
 * well: what the call put in and what its result held. A group that no handle
 * names any more is counted in the row of group id 0.
 */
struct CallStats {
    std::uint64_t group = 0;
    dstd::StringView kind;
    std::uint64_t started = 0;
    std::uint64_t ok = 0;
    std::uint64_t failed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
};

/// A call of a group's sequence of collectives that has not ended, at one
/// moment. @ref rank is the group rank that made it.
struct PendingCall {
    std::uint64_t group = 0;
    dstd::StringView kind;
    std::uint64_t seq = 0;
    std::uint32_t rank = 0;
    std::uint64_t age_ns = 0;
};

}  // namespace dftracer::utils::comm

#endif  // DFTRACER_UTILS_COMM_STATS_H
