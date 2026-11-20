// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BACKEND_LOADER_H
#define DFTRACER_UTILS_COMM_BACKEND_LOADER_H

#include <dftracer/utils/comm/backend/bootstrap.h>
#include <dftracer/utils/comm/backend/bulk.h>
#include <dftracer/utils/comm/backend/collectives.h>
#include <dftracer/utils/comm/backend/messaging.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::comm {

/// Which backend a communicator runs on.
enum class BackendKind : std::uint8_t {
    /// One rank in this process.
    Local,
    /// One rank per process over MPI, from the comm_mpi library.
    Mpi,
    /// Mpi when a launcher started this process, else Local.
    Auto,
};

/// What a backend library hands the communicator.
struct Providers {
    dstd::SharedPtr<backend::Bootstrap> bootstrap;
    dstd::SharedPtr<backend::Messaging> messaging;
    dstd::SharedPtr<backend::Collectives> collectives;
    /// Null makes the communicator run bulk transfers over @ref messaging.
    dstd::SharedPtr<backend::Bulk> bulk{};
};

/// What a start gives a backend library. @ref app_comm is null for the world of
/// the library, or points at the communicator handle of the application.
struct StartOptions {
    const void* app_comm = nullptr;
};

namespace backend {

/// What the one C entry function of a backend library returns. The library
/// must have been built with the same comm as the loader: both compare
/// @ref api_version and @ref build_stamp.
struct BackendEntry {
    const char* api_version = nullptr;
    const char* build_stamp = nullptr;
    dstd::Result<Providers> (*start)(const StartOptions& options) = nullptr;
};

/// The build stamp of this comm, which a backend library reports back.
const char* build_stamp() noexcept;

/// The name of the C entry function of the backend library.
inline constexpr const char* MPI_ENTRY_SYMBOL = "dftu_comm_mpi_entry";

/// True when the environment says that an MPI or PMIx launcher, or a job
/// manager with more than one task, started this process.
bool launched_by_mpi();

/// Resolves @p kind, so Auto becomes Local or Mpi.
BackendKind resolve_kind(BackendKind kind);

/// Loads the MPI backend library and starts it. The directory named by the
/// path variable is the only one searched when it is set; otherwise the
/// directory of comm itself comes first and the loader path second. A failure
/// names every path tried with its loader error. @p symbol is the entry symbol.
dstd::Result<Providers> load_backend(
    const StartOptions& options, dstd::CStringView symbol = MPI_ENTRY_SYMBOL);

}  // namespace backend

}  // namespace dftracer::utils::comm

#endif  // DFTRACER_UTILS_COMM_BACKEND_LOADER_H
