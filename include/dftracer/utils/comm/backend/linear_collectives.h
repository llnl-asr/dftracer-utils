// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BACKEND_LINEAR_COLLECTIVES_H
#define DFTRACER_UTILS_COMM_BACKEND_LINEAR_COLLECTIVES_H

#include <dftracer/utils/comm/backend/collectives.h>
#include <dftracer/utils/comm/backend/messaging.h>
#include <dftracer/utils/dstd/memory.h>

namespace dftracer::utils::comm::backend {

/// A @ref Collectives over any @ref Messaging. Every member sends its record to
/// group rank 0, which sends the table back to every member.
dstd::SharedPtr<Collectives> make_linear_collectives(
    dstd::SharedPtr<Messaging> messaging);

}  // namespace dftracer::utils::comm::backend

#endif  // DFTRACER_UTILS_COMM_BACKEND_LINEAR_COLLECTIVES_H
