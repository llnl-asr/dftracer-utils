// SPDX-License-Identifier: MIT
// A backend library built for another comm: its stamp is not the stamp of the
// comm that loads it, so the loader must refuse it.
#include <dftracer/utils/comm/backend/loader.h>
#include <dftracer/utils/common/versions.h>

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
        DFTRACER_UTILS_API_VERSION_STRING, "a-stamp-of-another-build",
        &never_started};
    return &entry;
}
