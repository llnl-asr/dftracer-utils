// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_MPI_COLLECTIVES_H
#define DFTRACER_UTILS_COMM_MPI_COLLECTIVES_H

#include <dftracer/utils/comm/backend/collectives.h>
#include <dftracer/utils/comm/mpi/context.h>
#include <dftracer/utils/dstd/memory.h>

namespace dftracer::utils::comm::mpi {

/**
 * @brief The @ref backend::Collectives of the MPI backend, over the
 * nonblocking collectives of MPI.
 *
 * Each group has a communicator of its own, made by MPI_Comm_create_group on
 * a thread of its own. The child of a split is made from the communicator of
 * its parent at the end of the split slot, while the parent runs no other
 * slot; the world and node groups are made at their first call. A group of
 * one member uses MPI_COMM_SELF. The communicator is freed when the group is
 * released and none of its requests is in flight. The calls of one group run
 * one slot at a time in sequence order, so every member posts the same
 * collectives in the same order on that communicator.
 *
 * A slot first exchanges the fixed record of every member and the size of its
 * contribution with MPI_Iallgather. Every member then holds the same records,
 * so all members choose one path from them and a call that the members made
 * differently ends with the records of all, as the caller expects. Then the
 * data moves:
 *
 * - a call with no table (barrier, split) ends after the records;
 * - a reduction with a built-in operation whose result is the same bit for bit
 *   as the fold in rank order runs MPI_Iallreduce or MPI_Ireduce: an integer
 *   type with Sum, Min or Max, or any type of a fixed width when the call
 *   allows any order. The table holds the fold as its only entry and
 *   @ref backend::SlotEnd::folded is set. A floating point Sum, Min or Max
 *   in rank order (Min and Max differ on NaN and signed zero), a custom
 *   operation and an object call gather every contribution and the caller
 *   folds them in rank order;
 * - broadcast and scatter run MPI_Ibcast from the root, gather runs
 *   MPI_Igatherv, and the others run MPI_Iallgatherv, into one buffer that the
 *   table views, with no copy at the receiver;
 * - a call with @ref backend::RECORD_PERSONAL runs MPI_Ialltoall for the
 *   sizes of the pieces and then MPI_Ialltoallv, so a member receives only the
 *   pieces addressed to it.
 *
 * A collective moves its data in rounds of at most 1 GiB, so a call has no
 * limit of the int count of MPI.
 *
 * A native request cannot be cancelled. An abort, a deadline or a cancel ends
 * the call at once with the error of the group and the request stays with the
 * context, so a buffer it uses stays alive until it completes. A deadline
 * asks the other members whether they reached the call and names those that
 * did not answer yes. When a context stops with a native request still pending
 * after a short wait, it ends the job with MPI_Abort and one line on stderr
 * instead of MPI_Finalize, so the job never hangs on a collective that a peer
 * never matched.
 */
dstd::SharedPtr<backend::Collectives> make_collectives(
    dstd::SharedPtr<Context> context);

}  // namespace dftracer::utils::comm::mpi

#endif  // DFTRACER_UTILS_COMM_MPI_COLLECTIVES_H
