// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_WORKER_SHARE_H
#define DFTRACER_UTILS_COMMON_WORKER_SHARE_H

#include <dftracer/utils/common/error.h>

#include <cstddef>

namespace dftracer::utils::common {

/// What one of several worker processes on this host may use.
struct WorkerShare {
    std::size_t threads = 1;
    std::size_t io_threads = 1;
    /// Bytes, for DFTRACER_UTILS_MEM_BUDGET.
    std::size_t memory_budget = 0;
};

/**
 * @brief One worker's share when `workers` processes split this host.
 *
 * threads and io_threads are the usable CPUs (@ref hardware_concurrency)
 * divided by `workers`, at least 1. memory_budget is the budget this process
 * would use (DFTRACER_UTILS_MEM_BUDGET when set, else a third of the available
 * memory) divided by `workers`, at least memory::MIN_MEMORY_BUDGET_BYTES. A
 * `workers` of 0 counts as 1. Fails when DFTRACER_UTILS_MEM_BUDGET is not a
 * byte size.
 *
 * A caller that applies the share by writing it back to
 * DFTRACER_UTILS_MEM_BUDGET passes `pin`: the first pinned call remembers the
 * budget this process had before, and every pinned call divides that one, so
 * applying a share again gives the same share and not a share of a share.
 */
dstd::Result<WorkerShare> worker_share(std::size_t workers, bool pin = false);

}  // namespace dftracer::utils::common

#endif  // DFTRACER_UTILS_COMMON_WORKER_SHARE_H
