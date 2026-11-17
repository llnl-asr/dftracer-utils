// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/loader.h>
#include <dftracer/utils/comm/mpi/c_api.h>
#include <dftracer/utils/comm/mpi/mpi_backend.h>
#include <dftracer/utils/common/versions.h>

#ifndef DFTRACER_UTILS_COMM_BUILD_STAMP
#error "the build must define DFTRACER_UTILS_COMM_BUILD_STAMP for comm_mpi"
#endif

extern "C" const dftracer::utils::comm::backend::BackendEntry*
dftu_comm_mpi_entry() {
    static const dftracer::utils::comm::backend::BackendEntry entry{
        DFTRACER_UTILS_API_VERSION_STRING, DFTRACER_UTILS_COMM_BUILD_STAMP,
        &dftracer::utils::comm::mpi::start};
    return &entry;
}
