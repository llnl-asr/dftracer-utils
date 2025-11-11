#include <dftracer/utils/core/pipeline/io_backend.h>

// Include platform-specific backends
#include <dftracer/utils/core/pipeline/io_backends/thread_pool_backend.h>

#ifdef __linux__
#include <dftracer/utils/core/pipeline/io_backends/io_uring_backend.h>
#endif

#ifdef __APPLE__
#include <dftracer/utils/core/pipeline/io_backends/kqueue_backend.h>
#endif

#include <memory>

namespace dftracer::utils {

// ============================================================================
// Factory Methods
// ============================================================================

std::unique_ptr<IOBackend> IOBackend::create() {
#ifdef __linux__
    // Try io_uring first (if available)
    auto io_uring = std::make_unique<IOUringBackend>();
    if (io_uring->is_available()) {
        return io_uring;
    }
#elif defined(__APPLE__)
    // Use kqueue on macOS/BSD
    return std::make_unique<KqueueBackend>();
#endif

    // Fallback to thread pool backend
    return std::make_unique<ThreadPoolIOBackend>();
}

bool IOBackend::is_available_on_platform() {
    // Thread pool fallback is always available on all platforms
    return true;
}

}  // namespace dftracer::utils
