// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_IO_BACKEND_TYPE_H
#define DFTRACER_UTILS_RUNTIME_IO_BACKEND_TYPE_H

namespace dftracer::utils::runtime::io {

/// Backend selection preference.
enum class IoBackendType {
    Auto,     ///< Runtime detection: io_uring > epoll/kqueue+threadpool >
              ///< threadpool
    IoUring,  ///< Force io_uring (Linux only, fails if unavailable)
    EpollThreadpool,   ///< Force epoll + thread pool (Linux only)
    KqueueThreadpool,  ///< Force kqueue + thread pool (macOS/BSD only)
    Threadpool         ///< Force pure thread pool
};

}  // namespace dftracer::utils::runtime::io

#endif  // DFTRACER_UTILS_RUNTIME_IO_BACKEND_TYPE_H
