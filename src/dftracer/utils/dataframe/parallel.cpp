#include <dftracer/utils/dataframe/parallel.h>

#include <atomic>

namespace dftracer::utils::dataframe {

namespace {
std::atomic<ParallelForFn> g_backend{nullptr};
void* g_ctx = nullptr;
}  // namespace

void set_parallel_backend(ParallelForFn fn, void* ctx) {
    g_ctx = ctx;
    g_backend.store(fn, std::memory_order_release);
}

bool parallel_backend_installed() {
    return g_backend.load(std::memory_order_acquire) != nullptr;
}

namespace detail {
void parallel_for_dispatch(std::int64_t n, std::int64_t grain,
                           void (*body)(void*, std::int64_t, std::int64_t),
                           void* body_ctx) {
    if (n <= 0) return;
    ParallelForFn fn = g_backend.load(std::memory_order_acquire);
    if (fn == nullptr || n <= grain) {
        body(body_ctx, 0, n);
        return;
    }
    fn(g_ctx, n, grain, body, body_ctx);
}
}  // namespace detail

}  // namespace dftracer::utils::dataframe
