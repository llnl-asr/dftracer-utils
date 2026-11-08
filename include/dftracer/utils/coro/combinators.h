// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_COMBINATORS_H
#define DFTRACER_UTILS_CORO_COMBINATORS_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/cancelled.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/spawn_future.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

namespace detail {

dstd::Error error_from_exception(const std::exception_ptr& error) noexcept;

/// The items claim their index one at a time, so a worker frame exists only for
/// a job that can run.
std::size_t parallel_worker_count(std::size_t requested, std::size_t items,
                                  const Executor* executor) noexcept;

template <typename T>
struct FirstSuccessOutcome {
    dstd::Optional<T> value;
    std::exception_ptr error;
};

template <typename T>
CoroTask<void> first_success_child(CoroTask<T> task,
                                   Channel<FirstSuccessOutcome<T>>& outcomes) {
    FirstSuccessOutcome<T> outcome;
    try {
        T value = co_await std::move(task);
        outcome.value.emplace(std::move(value));
    } catch (...) {
        outcome.error = std::current_exception();
    }
    // The channel has one slot per child, so this send cannot be refused.
    outcomes.try_send(std::move(outcome));
}

// The waits of these two run in their own task, because a cancelled wait ends
// its frame, and the frame that holds the scope must reach the join.
template <typename T>
CoroTask<void> first_success_collect(Channel<FirstSuccessOutcome<T>>& outcomes,
                                     std::size_t total,
                                     dstd::Optional<T>& winner,
                                     std::exception_ptr& last_error) {
    for (std::size_t seen = 0; seen < total && !winner; ++seen) {
        auto outcome = co_await outcomes.receive();
        if (outcome->value) {
            winner = std::move(outcome->value);
        } else {
            last_error = std::move(outcome->error);
        }
    }
}

template <typename T>
CoroTask<void> settled_collect(
    dstd::Vector<SpawnFuture<T>>& futures,
    dstd::Vector<dstd::Expected<T, dstd::Error>>& outcomes) {
    for (auto& future : futures) {
        std::exception_ptr error;
        try {
            if constexpr (std::is_void_v<T>) {
                co_await future;
                outcomes.emplace_back();
            } else {
                T value = co_await future;
                outcomes.emplace_back(std::move(value));
            }
        } catch (...) {
            error = std::current_exception();
        }
        if (error) {
            outcomes.push_back(dstd::Unexpected(error_from_exception(error)));
        }
    }
}

template <typename T, typename Fn, typename R>
struct ParallelMapState {
    ParallelMapState(dstd::Vector<T> input, Fn function)
        : items(std::move(input)), fn(std::move(function)) {
        results.resize(items.size());
    }

    dstd::Vector<T> items;
    Fn fn;
    std::atomic<std::size_t> next{0};
    dstd::Vector<dstd::Optional<R>> results;
};

template <typename T, typename Fn>
struct ParallelMapState<T, Fn, void> {
    ParallelMapState(dstd::Vector<T> input, Fn function)
        : items(std::move(input)), fn(std::move(function)) {}

    dstd::Vector<T> items;
    Fn fn;
    std::atomic<std::size_t> next{0};
};

template <typename T, typename Fn, typename R>
CoroTask<void> parallel_map_worker(ParallelMapState<T, Fn, R>& state,
                                   const Scope& scope) {
    for (;;) {
        if (scope.stop_requested()) co_return;
        const std::size_t index =
            state.next.fetch_add(1, std::memory_order_relaxed);
        if (index >= state.items.size()) co_return;
        if constexpr (std::is_void_v<R>) {
            co_await std::invoke(state.fn, std::move(state.items[index]));
        } else {
            R value =
                co_await std::invoke(state.fn, std::move(state.items[index]));
            state.results[index].emplace(std::move(value));
        }
    }
}

template <typename R>
struct UnorderedMessage {
    UnorderedMessage() = default;
    UnorderedMessage(UnorderedMessage&&) = default;
    UnorderedMessage& operator=(UnorderedMessage&&) = default;
    // A message no reader takes, as when the generator is dropped, is dropped
    // on purpose, an error result in it too.
    ~UnorderedMessage() { dstd::discard_unread(value); }

    dstd::Optional<R> value;
    std::exception_ptr error;
};

template <typename T, typename Fn, typename R>
struct UnorderedState {
    UnorderedState(dstd::Vector<T> input, Fn function, std::size_t capacity)
        : items(std::move(input)), fn(std::move(function)), channel(capacity) {}

    dstd::Vector<T> items;
    Fn fn;
    std::atomic<std::size_t> next{0};
    Channel<UnorderedMessage<R>> channel;
};

// Owns its state, because the generator can be dropped while this runs. It ends
// at its next send once the generator closes the channel.
template <typename T, typename Fn, typename R>
Coro unordered_worker(dstd::SharedPtr<UnorderedState<T, Fn, R>> state,
                      ChannelProducer<UnorderedMessage<R>> producer) {
    auto guard = producer.guard();
    try {
        for (;;) {
            const std::size_t index =
                state->next.fetch_add(1, std::memory_order_relaxed);
            if (index >= state->items.size()) co_return;
            UnorderedMessage<R> message;
            try {
                R value = co_await std::invoke(state->fn,
                                               std::move(state->items[index]));
                message.value.emplace(std::move(value));
            } catch (...) {
                message.error = std::current_exception();
            }
            const bool failed = message.error != nullptr;
            const bool sent = co_await state->channel.send(std::move(message));
            if (failed || !sent) co_return;
        }
    } catch (...) {
        // A wait cancelled at shutdown. The generator sees the channel end.
    }
}

}  // namespace detail

/**
 * @brief Waits for every task and gives each outcome, with errors as values.
 *
 * Tasks run at once on the current executor. A failed task does not stop the
 * others.
 */
template <typename T>
CoroTask<dstd::Vector<dstd::Expected<T, dstd::Error>>> when_all_settled(
    dstd::Vector<CoroTask<T>> tasks) {
    const PromiseBase* owner = co_await detail::OWNS_SCOPE;
    Executor* executor = co_await this_coro::executor;
    Scope scope(executor, owner);
    dstd::Vector<SpawnFuture<T>> futures;
    futures.reserve(tasks.size());
    for (auto& task : tasks) {
        futures.push_back(scope.spawn_future(std::move(task)));
    }
    dstd::Vector<dstd::Expected<T, dstd::Error>> outcomes;
    outcomes.reserve(futures.size());
    std::exception_ptr cancelled;
    try {
        co_await detail::settled_collect(futures, outcomes);
    } catch (...) {
        cancelled = std::current_exception();
    }
    if (cancelled) {
        for (auto& future : futures) future.cancel();
    }
    co_await scope.join();
    if (cancelled) {
        for (const auto& outcome : outcomes) outcome.discard();
        common::rethrow_and_clear(cancelled);
    }
    co_return outcomes;
}

/**
 * @brief Gives the value of the first task that succeeds, cancels the others at
 * their next wait and waits for them to end.
 *
 * Raises the last error when all fail, and an invalid-argument error when
 * @p tasks is empty.
 */
template <typename T>
    requires(!std::is_void_v<T>)
CoroTask<T> first_success(dstd::Vector<CoroTask<T>> tasks) {
    if (tasks.empty()) {
        throw common::DFTUtilsException(
            dstd::Error{dstd::Condition::InvalidArgument,
                        "first_success needs at least one task"});
    }
    const PromiseBase* owner = co_await detail::OWNS_SCOPE;
    Executor* executor = co_await this_coro::executor;
    const std::size_t total = tasks.size();
    Channel<detail::FirstSuccessOutcome<T>> outcomes(total);
    Scope scope(executor, owner);
    dstd::Vector<SpawnFuture<void>> futures;
    futures.reserve(total);
    for (auto& task : tasks) {
        futures.push_back(scope.spawn_future(
            detail::first_success_child(std::move(task), outcomes)));
    }
    dstd::Optional<T> winner;
    std::exception_ptr last_error;
    std::exception_ptr cancelled;
    try {
        co_await detail::first_success_collect(outcomes, total, winner,
                                               last_error);
    } catch (...) {
        cancelled = std::current_exception();
    }
    if (winner || cancelled) {
        for (auto& future : futures) future.cancel();
    }
    co_await scope.join();
    if (cancelled) common::rethrow_and_clear(cancelled);
    if (winner) co_return std::move(*winner);
    common::rethrow_and_clear(last_error);
}

struct ParallelOptions {
    /// Jobs that run at once. 0 uses the concurrency of the executor.
    std::size_t max_in_flight{0};
};

template <typename T, typename Fn>
using ParallelMapValue = typename std::invoke_result_t<Fn&, T&&>::value_type;

template <typename R>
using ParallelMapResult =
    std::conditional_t<std::is_void_v<R>, void, dstd::Vector<R>>;

/**
 * @brief Runs `fn(item)` for each item, at most `max_in_flight` at a time, and
 * gives the results in item order.
 *
 * Fails fast: the first error ends running jobs as cancelled at their next
 * wait, and that error is raised. Jobs that do not start are never called.
 */
template <typename T, typename Fn, typename R = ParallelMapValue<T, Fn>>
CoroTask<ParallelMapResult<R>> parallel_map(dstd::Vector<T> items, Fn fn,
                                            ParallelOptions options = {}) {
    const PromiseBase* owner = co_await detail::OWNS_SCOPE;
    Executor* executor = co_await this_coro::executor;
    const std::size_t workers = detail::parallel_worker_count(
        options.max_in_flight, items.size(), executor);
    detail::ParallelMapState<T, Fn, R> state(std::move(items), std::move(fn));
    Scope scope(executor, owner);
    for (std::size_t i = 0; i < workers; ++i) {
        scope.spawn(detail::parallel_map_worker(state, scope));
    }
    co_await scope.join();
    // A worker stops without an error on a stop from outside.
    if (state.next.load(std::memory_order_relaxed) < state.items.size()) {
        throw_cancelled("parallel_map stopped");
    }
    if constexpr (!std::is_void_v<R>) {
        dstd::Vector<R> results;
        results.reserve(state.results.size());
        for (auto& result : state.results) {
            results.push_back(std::move(*result));
        }
        co_return results;
    }
}

/**
 * @brief Runs `fn(item)` for each item, at most `max_in_flight` at a time, and
 * yields each result as it finishes.
 *
 * Fails fast: the first error is raised from the next read and later jobs do
 * not start. Dropping the generator stops new jobs. A running job ends in the
 * background and owns @p fn and the items until then, so @p fn must capture by
 * value. Jobs finished but not read wait in a channel of `max_in_flight` slots,
 * which holds back the next ones.
 */
template <typename T, typename Fn, typename R = ParallelMapValue<T, Fn>>
    requires(!std::is_void_v<R>)
AsyncGenerator<R> parallel_map_unordered(dstd::Vector<T> items, Fn fn,
                                         ParallelOptions options = {}) {
    Executor* executor = co_await this_coro::executor;
    if (executor == nullptr) {
        throw common::DFTUtilsException(
            dstd::Error{dstd::Condition::InvalidArgument,
                        "parallel_map_unordered needs a running executor"});
    }
    const std::size_t workers = detail::parallel_worker_count(
        options.max_in_flight, items.size(), executor);
    if (workers == 0) co_return;

    using State = detail::UnorderedState<T, Fn, R>;
    auto state =
        dstd::make_shared<State>(std::move(items), std::move(fn), workers);
    for (std::size_t i = 0; i < workers; ++i) {
        detail::Coro worker = detail::unordered_worker<T, Fn, R>(
            state, state->channel.producer());
        worker.handle().promise().executor = executor;
        executor->post(worker.release());
    }
    struct Closer {
        State& state;
        ~Closer() { state.channel.close(); }
    } closer{*state};
    for (;;) {
        auto message = co_await state->channel.receive();
        if (!message) break;
        if (message->error) {
            state->next.store(state->items.size(), std::memory_order_relaxed);
            common::rethrow_and_clear(message->error);
        }
        co_yield std::move(*message->value);
    }
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_COMBINATORS_H
