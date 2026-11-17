// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_MPI_C_API_H
#define DFTRACER_UTILS_COMM_MPI_C_API_H

#include <dftracer/utils/comm/backend/loader.h>

/// The one symbol the loader of comm resolves in the MPI backend library: the
/// entry that carries the versions of the comm the library was built with and
/// the function that starts the providers.
extern "C" const dftracer::utils::comm::backend::BackendEntry*
dftu_comm_mpi_entry();

#endif  // DFTRACER_UTILS_COMM_MPI_C_API_H
