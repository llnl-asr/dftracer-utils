// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_PARALLEL_H
#define DFTRACER_UTILS_CORO_PARALLEL_H

#include <dftracer/utils/coro/cancelled.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftracer::utils::coro {

/// How @ref parallel_for and its relatives cut a range.
enum class Partition {
    /// Workers claim chunks of `grain` items, which balances uneven work.
    Dynamic,
    /// One equal chunk per worker. The same range and `max_in_flight` always
    /// give the same chunks.
    Static,
};

struct ParallelForOptions {
    /// The smallest chunk, at least 1. It also caps the workers of a static
    /// partition.
    std::int64_t grain{1};
    /// Chunks that run at once. 0 uses the concurrency of the executor.
    std::size_t max_in_flight{0};
    Partition partition{Partition::Dynamic};
};

namespace detail {

struct ChunkPlan {
    std::int64_t size{0};
    std::int64_t grain{1};
    std::size_t chunks{0};
    std::size_t workers{0};
    Partition partition{Partition::Dynamic};

    /// Raises an invalid-argument error when the grain is below 1.
    static ChunkPlan make(std::int64_t size, const ParallelForOptions& options,
                          const Executor* executor);

    std::int64_t begin(std::size_t chunk) const noexcept;
    std::int64_t end(std::size_t chunk) const noexcept;
};

template <class F>
struct ChunkState {
    const ChunkPlan& plan;
    F& fn;
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> ran{0};
};

template <class F>
CoroTask<void> chunk_worker(ChunkState<F>& state, const Scope& scope,
                            std::size_t worker) {
    const bool is_static = state.plan.partition == Partition::Static;
    for (;;) {
        if (scope.stop_requested()) co_return;
        const std::size_t chunk =
            is_static ? worker
                      : state.next.fetch_add(1, std::memory_order_relaxed);
        if (chunk >= state.plan.chunks) co_return;
        state.fn(chunk, state.plan.begin(chunk), state.plan.end(chunk));
        state.ran.fetch_add(1, std::memory_order_relaxed);
        if (is_static) co_return;
    }
}

template <class F>
CoroTask<void> run_chunks(const ChunkPlan& plan, F& fn) {
    const PromiseBase* owner = co_await detail::OWNS_SCOPE;
    Executor* executor = co_await this_coro::executor;
    ChunkState<F> state{plan, fn};
    Scope scope(executor, owner);
    for (std::size_t worker = 0; worker < plan.workers; ++worker) {
        scope.spawn(chunk_worker(state, scope, worker));
    }
    co_await scope.join();
    // A worker stops without an error on a stop from outside.
    if (state.ran.load(std::memory_order_relaxed) < plan.chunks) {
        throw_cancelled("parallel range stopped");
    }
}

}  // namespace detail

/**
 * @brief Runs `body(begin, end)` over parts of [0, @p size) and waits without
 * holding a thread.
 *
 * `body` runs at the same time on separate parts and does not wait, so a long
 * part holds its worker. A stop or a cancel starts no more parts. The first
 * error is raised after the running parts end. Raises an invalid-argument error
 * on a grain below 1.
 */
template <class Body>
CoroTask<void> parallel_for(std::int64_t size, Body body,
                            ParallelForOptions options = {}) {
    Executor* executor = co_await this_coro::executor;
    const auto plan = detail::ChunkPlan::make(size, options, executor);
    auto run = [&body](std::size_t, std::int64_t begin, std::int64_t end) {
        body(begin, end);
    };
    auto chunks = detail::run_chunks(plan, run);
    co_await std::move(chunks);
}

/**
 * @brief Folds `map(begin, end)` of parts of [0, @p size) with `combine`, in
 * range order, so `combine` must be associative and need not be commutative.
 *
 * Stop and error behavior are those of @ref parallel_for. Gives @p identity on
 * an empty range.
 */
template <class T, class Map, class Combine>
CoroTask<T> parallel_reduce(std::int64_t size, T identity, Map map,
                            Combine combine, ParallelForOptions options = {}) {
    Executor* executor = co_await this_coro::executor;
    const auto plan = detail::ChunkPlan::make(size, options, executor);
    dstd::Vector<dstd::Optional<T>> partials(plan.chunks);
    auto run = [&map, &partials](std::size_t chunk, std::int64_t begin,
                                 std::int64_t end) {
        partials[chunk].emplace(map(begin, end));
    };
    auto chunks = detail::run_chunks(plan, run);
    co_await std::move(chunks);
    T result = std::move(identity);
    for (auto& partial : partials) {
        result = combine(std::move(result), std::move(*partial));
    }
    co_return result;
}

/**
 * @brief A parallel prefix scan over [0, @p size), and the total as its result.
 *
 * `scan(begin, end, start, is_final)` folds a part from `start` and returns the
 * sum. It runs twice per part: first with @p identity and `is_final` false to
 * learn the part's sum, then with the sum of all earlier parts and `is_final`
 * true, when it writes its output. `combine` is associative. Stop and error
 * behavior are those of @ref parallel_for.
 */
template <class T, class Scan, class Combine>
CoroTask<T> parallel_scan(std::int64_t size, T identity, Scan scan,
                          Combine combine, ParallelForOptions options = {}) {
    Executor* executor = co_await this_coro::executor;
    const auto plan = detail::ChunkPlan::make(size, options, executor);
    if (plan.chunks == 0) co_return identity;
    dstd::Vector<dstd::Optional<T>> partials(plan.chunks);
    auto first = [&](std::size_t chunk, std::int64_t begin, std::int64_t end) {
        partials[chunk].emplace(scan(begin, end, identity, false));
    };
    auto first_pass = detail::run_chunks(plan, first);
    co_await std::move(first_pass);

    dstd::Vector<T> starts;
    starts.reserve(plan.chunks);
    starts.push_back(identity);
    for (std::size_t chunk = 1; chunk < plan.chunks; ++chunk) {
        starts.push_back(combine(starts.back(), *partials[chunk - 1]));
    }
    auto last = [&](std::size_t chunk, std::int64_t begin, std::int64_t end) {
        scan(begin, end, starts[chunk], true);
    };
    auto last_pass = detail::run_chunks(plan, last);
    co_await std::move(last_pass);
    co_return combine(starts.back(), *partials.back());
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_PARALLEL_H
