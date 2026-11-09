// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/runtime/runtime.h>

// Bridges the dataframe parallel_for seam to the core coroutine runtime's
// thread pool. Kept in its own TU so the seam header stays runtime-free; a
// consumer opts in by calling install_runtime_parallel_backend().
namespace dftracer::utils::dataframe {

namespace {
void runtime_backend(void*, std::int64_t n, std::int64_t grain,
                     void (*body)(void*, std::int64_t, std::int64_t),
                     void* bctx) {
    // The seam has no way to report a fork-join that did not run, and a result
    // built from a partial run would be wrong.
    const auto ran = default_runtime().parallel_for(
        n, grain, [&](std::int64_t b, std::int64_t e) { body(bctx, b, e); });
    if (!ran) {
        dstd::fatal("parallel_for: the runtime stopped during a fork-join");
    }
}
}  // namespace

void install_runtime_parallel_backend() {
    set_parallel_backend(runtime_backend, nullptr);
}

}  // namespace dftracer::utils::dataframe
