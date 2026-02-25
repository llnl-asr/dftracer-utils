#pragma once

#include <dftracer/utils/core/io/io_backend.h>

#include <cstddef>
#include <memory>

namespace dftracer::utils {
class Executor;
}

namespace dftracer::utils::io {

/// Create the best available I/O backend for this system.
/// @param executor          The executor that owns this backend.
/// @param pool_size         Number of threads for the I/O thread pool.
/// @param backend_type      Backend selection (AUTO = runtime detection).
/// @param batch_threshold   SQE batch threshold (io_uring), 16 default.
std::unique_ptr<IoBackend> create_io_backend(
    Executor& executor, std::size_t pool_size = 4,
    IoBackendType backend_type = IoBackendType::AUTO,
    unsigned batch_threshold = 16);

}  // namespace dftracer::utils::io
