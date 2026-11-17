// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_MPI_MPI_BACKEND_H
#define DFTRACER_UTILS_COMM_MPI_MPI_BACKEND_H

#include <dftracer/utils/comm/backend/loader.h>
#include <dftracer/utils/comm/mpi/context.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>

namespace dftracer::utils::comm::mpi {

/// The bootstrap of the ranks of @p context: one rank per process.
dstd::SharedPtr<backend::Bootstrap> make_bootstrap(
    dstd::SharedPtr<Context> context);

/// The messaging between the processes of @p context. A send to another
/// process goes over MPI and ends when the receiving process stored or paired
/// the message; every other call runs on the matching engine of this process.
dstd::SharedPtr<backend::Messaging> make_messaging(
    dstd::SharedPtr<Context> context);

/// Starts the MPI backend on the communicator in @p options.
dstd::Result<Providers> start(const StartOptions& options);

}  // namespace dftracer::utils::comm::mpi

#endif  // DFTRACER_UTILS_COMM_MPI_MPI_BACKEND_H
