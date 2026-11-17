// SPDX-License-Identifier: MIT
// A backend library with the stamp of this build and an API version that no
// comm has: the loader must refuse it.
#include <dftracer/utils/comm/backend/loader.h>

namespace {

dftracer::utils::dstd::Result<dftracer::utils::comm::Providers> never_started(
    const dftracer::utils::comm::StartOptions&) {
    return dftracer::utils::dstd::Unexpected(dftracer::utils::dstd::Error(
        dftracer::utils::dstd::Condition::Internal, "the fake never starts"));
}

}  // namespace

extern "C" const dftracer::utils::comm::backend::BackendEntry*
dftu_comm_mpi_entry() {
    static const dftracer::utils::comm::backend::BackendEntry entry{
        "9.9.9", DFTU_FAKE_STAMP, &never_started};
    return &entry;
}
