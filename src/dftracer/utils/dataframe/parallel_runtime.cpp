#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/parallel.h>

// Bridges the dataframe parallel_for seam to the core coroutine runtime's
// thread pool. Kept in its own TU so the seam header stays runtime-free; a
// consumer opts in by calling install_runtime_parallel_backend().
namespace dftracer::utils::dataframe {

namespace {
void runtime_backend(void*, std::int64_t n, std::int64_t grain,
                     void (*body)(void*, std::int64_t, std::int64_t),
                     void* bctx) {
    default_runtime().parallel_for(
        n, grain, [&](std::int64_t b, std::int64_t e) { body(bctx, b, e); });
}
}  // namespace

void install_runtime_parallel_backend() {
    set_parallel_backend(runtime_backend, nullptr);
}

}  // namespace dftracer::utils::dataframe
