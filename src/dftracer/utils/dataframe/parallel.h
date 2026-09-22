#ifndef DFTRACER_UTILS_DATAFRAME_PARALLEL_H
#define DFTRACER_UTILS_DATAFRAME_PARALLEL_H

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

// A parallelism seam for the dataframe kernels. dataframe is a leaf library
// with no runtime dependency, so it runs serial by default; a host (the core
// coroutine runtime, or a distributed engine) installs a backend once at
// startup, and data-parallel kernels then fan out through it. This keeps
// dataframe's C ABI clean and portable while letting the same kernels
// parallelize wherever a backend is present.
namespace dftracer::utils::dataframe {

/// A backend runs `body(body_ctx, begin, end)` over disjoint sub-ranges
/// covering [0, n), possibly concurrently, and returns only when all have
/// finished. `grain` is the chunk size the caller suggests.
using ParallelForFn = void (*)(void* ctx, std::int64_t n, std::int64_t grain,
                               void (*body)(void* body_ctx, std::int64_t begin,
                                            std::int64_t end),
                               void* body_ctx);

/// Install the backend (nullptr restores serial). Set once at startup.
void set_parallel_backend(ParallelForFn fn, void* ctx);

/// Whether a backend is installed. Lets an op skip a parallel algorithm whose
/// extra work (a merge pass) is pure overhead when everything would run serial.
bool parallel_backend_installed();

/// Install a backend that fans work out through the core coroutine runtime's
/// thread pool (default_runtime()). One call at startup makes the data-parallel
/// dataframe kernels (group-by, expr eval, ...) run multi-threaded on the C++
/// path, mirroring what the Python extension installs. Opt-in: the engine stays
/// serial until this (or another backend) is set.
void install_runtime_parallel_backend();

namespace detail {
/// Dispatch through the backend, or run serial when none is installed or
/// `n <= grain` (so small work never pays the fan-out cost).
void parallel_for_dispatch(std::int64_t n, std::int64_t grain,
                           void (*body)(void*, std::int64_t, std::int64_t),
                           void* body_ctx);
}  // namespace detail

/// Run `body(begin, end)` over [0, n) in grain-sized chunks. Blocks until all
/// chunks finish. `body` must be safe to run concurrently on disjoint ranges.
template <class Body>
void parallel_for(std::int64_t n, std::int64_t grain, Body&& body) {
    using B = std::remove_reference_t<Body>;
    auto thunk = [](void* p, std::int64_t begin, std::int64_t end) {
        (*static_cast<B*>(p))(begin, end);
    };
    detail::parallel_for_dispatch(n, grain, thunk, &body);
}

/// Data-parallel reduction over [0, n): `map(begin, end)` produces a partial
/// per chunk, combined pairwise by `combine`. `identity` must be a neutral
/// element for `combine`. `T` must be copyable.
template <class T, class Map, class Combine>
T parallel_reduce(std::int64_t n, std::int64_t grain, T identity, Map&& map,
                  Combine&& combine) {
    if (n <= 0) return identity;
    const std::int64_t chunks = (n + grain - 1) / grain;
    if (chunks <= 1) return combine(std::move(identity), map(0, n));
    std::vector<T> partials(static_cast<std::size_t>(chunks), identity);
    parallel_for(n, grain, [&](std::int64_t b, std::int64_t e) {
        partials[static_cast<std::size_t>(b / grain)] = map(b, e);
    });
    T acc = std::move(identity);
    for (T& p : partials) acc = combine(std::move(acc), std::move(p));
    return acc;
}

/// Two-pass parallel prefix scan over [0, n) for an associative op (cumsum,
/// cummax, cummin, cumprod, ...). `local(begin, end)` computes the chunk's
/// local scan (as if it were the whole array, seeded from `identity`) and
/// returns the chunk's final accumulated value. `apply(begin, end, offset)`
/// folds the preceding chunks' combined result into an already-scanned chunk.
/// `combine` folds two chunk totals together and must share the same
/// identity element as `local`'s seed. Falls back to one `local(0, n)` call
/// when no backend is installed or `n <= grain`.
template <class T, class Local, class Apply, class Combine>
void parallel_prefix_scan(std::int64_t n, std::int64_t grain, T identity,
                          Local&& local, Apply&& apply, Combine&& combine) {
    if (n <= 0) return;
    if (!parallel_backend_installed() || n <= grain) {
        local(0, n);
        return;
    }
    const std::int64_t chunks = (n + grain - 1) / grain;
    std::vector<T> totals(static_cast<std::size_t>(chunks));
    parallel_for(n, grain, [&](std::int64_t b, std::int64_t e) {
        totals[static_cast<std::size_t>(b / grain)] = local(b, e);
    });
    std::vector<T> offsets(static_cast<std::size_t>(chunks));
    T acc = identity;
    for (std::int64_t c = 0; c < chunks; ++c) {
        offsets[static_cast<std::size_t>(c)] = acc;
        acc = combine(acc, totals[static_cast<std::size_t>(c)]);
    }
    parallel_for(n, grain, [&](std::int64_t b, std::int64_t e) {
        apply(b, e, offsets[static_cast<std::size_t>(b / grain)]);
    });
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_PARALLEL_H
