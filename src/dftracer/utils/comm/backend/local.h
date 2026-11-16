// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BACKEND_LOCAL_H
#define DFTRACER_UTILS_COMM_BACKEND_LOCAL_H

#include <dftracer/utils/comm/backend/bootstrap.h>
#include <dftracer/utils/comm/backend/collectives.h>
#include <dftracer/utils/comm/backend/messaging.h>
#include <dftracer/utils/dstd/memory.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::coro {
class Executor;
}  // namespace dftracer::utils::coro

namespace dftracer::utils::comm::backend {

/// The providers of a run whose ranks are coroutines of this process. A send
/// hands its bytes to a parked receive, and a collection reads the records of
/// every member under one lock.
dstd::SharedPtr<Bootstrap> make_local_bootstrap(std::uint32_t ranks,
                                                coro::Executor& executor);
dstd::SharedPtr<Messaging> make_local_messaging();
dstd::SharedPtr<Collectives> make_local_collectives();

/// What a local messaging provider still holds.
struct LocalCounts {
    /// Streams it keeps, one per pair of ranks and tag.
    std::size_t streams = 0;
    /// Messages parked in those streams.
    std::size_t parked = 0;
    /// Groups it remembers an end of, which a release erases.
    std::size_t ended = 0;
    /// Ranges of released group ids it remembers, which neighbours coalesce.
    std::size_t released_ranges = 0;
};

/// What @p messaging holds, which a test reads to prove it releases a group.
LocalCounts local_counts(const Messaging& messaging);

}  // namespace dftracer::utils::comm::backend

#endif  // DFTRACER_UTILS_COMM_BACKEND_LOCAL_H
