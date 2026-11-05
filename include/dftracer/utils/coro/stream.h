// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_STREAM_H
#define DFTRACER_UTILS_CORO_STREAM_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/adaptor.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/generator.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

namespace detail {

template <typename T>
struct StreamMessage {
    dstd::Optional<T> value;
    std::exception_ptr error;
};

// What the pumps share with the generator that reads them. The pumps own it,
// because the generator can be dropped while they run.
template <typename T>
struct StreamShared {
    StreamShared(std::size_t capacity, Executor* exec)
        : channel(capacity), executor(exec) {}

    /// Ends the pumps: a send sees the closed channel and a wait in a source
    /// sees the flag.
    void stop() noexcept {
        stopped.store(true, std::memory_order_release);
        channel.close();
        wake_cancelled_on(executor);
    }

    Channel<StreamMessage<T>> channel;
    Executor* executor;
    std::atomic<bool> stopped{false};
};

// A stream read to its end has no pump left, so it skips the wake.
template <typename T>
struct StreamCloser {
    StreamShared<T>& shared;
    bool drained{false};
    ~StreamCloser() {
        if (!drained) shared.stop();
    }
};

inline void start_stream_pump(Coro pump, Executor* executor) {
    pump.handle().promise().executor = executor;
    executor->post(pump.release());
}

inline Executor* require_stream_executor(Executor* executor) {
    if (executor == nullptr) {
        throw common::DFTUtilsException(
            dstd::Error{dstd::Condition::InvalidArgument,
                        "a merged stream needs a running executor"});
    }
    return executor;
}

template <typename T, typename Shared>
Coro pump_stream(dstd::SharedPtr<Shared> shared,
                 ChannelProducer<StreamMessage<T>> producer,
                 AsyncGenerator<T> source, AsyncSemaphore* slot = nullptr) {
    source.stop_on(&shared->stopped);
    auto guard = producer.guard();
    try {
        for (;;) {
            StreamMessage<T> message;
            bool done = false;
            try {
                auto value = co_await source.next();
                if (value) {
                    message.value.emplace(std::move(*value));
                } else {
                    done = true;
                }
            } catch (...) {
                message.error = std::current_exception();
            }
            if (done) break;
            const bool failed = message.error != nullptr;
            const bool sent = co_await shared->channel.send(std::move(message));
            if (failed || !sent) break;
        }
    } catch (...) {
        // A wait cancelled at shutdown. The reader sees the channel end.
    }
    if (slot) slot->release(1);
}

template <typename T>
AsyncGenerator<T> read_stream(dstd::SharedPtr<StreamShared<T>> shared) {
    StreamCloser<T> closer{*shared};
    for (;;) {
        auto message = co_await shared->channel.receive();
        if (!message) {
            closer.drained = true;
            co_return;
        }
        if (message->error) common::rethrow_and_clear(message->error);
        co_yield std::move(*message->value);
    }
}

// Sends a source error after any wait: the channel can be full.
template <typename T>
Coro send_stream_error(dstd::SharedPtr<StreamShared<T>> shared,
                       ChannelProducer<StreamMessage<T>> producer,
                       std::exception_ptr error) {
    auto guard = producer.guard();
    StreamMessage<T> message;
    message.error = std::move(error);
    try {
        co_await shared->channel.send(std::move(message));
    } catch (...) {
        // A wait cancelled at shutdown. The reader sees the channel end.
    }
}

// Holds fn because a coroutine lambda reads its captures through the closure,
// and its generators outlive the outer pump.
template <typename T, typename F>
struct MergeMapShared : StreamShared<T> {
    MergeMapShared(std::size_t limit, Executor* exec, F function)
        : StreamShared<T>(limit, exec), slots(limit), fn(std::move(function)) {}
    AsyncSemaphore slots;
    F fn;
};

template <typename U, typename F, typename T>
CoroTask<U> call_as_task(F& fn, T value) {
    using R = std::invoke_result_t<F&, T&&>;
    if constexpr (is_coro_task_v<R>) {
        U result = co_await std::invoke(fn, std::move(value));
        co_return std::move(result);
    } else {
        co_return std::invoke(fn, std::move(value));
    }
}

template <typename S, typename F, typename U>
Coro pump_outer(dstd::SharedPtr<MergeMapShared<U, F>> shared,
                ChannelProducer<StreamMessage<U>> producer,
                AsyncGenerator<S> source) {
    using Inner = AsyncGenerator<U>;
    source.stop_on(&shared->stopped);
    auto guard = producer.guard();
    std::exception_ptr error;
    try {
        for (;;) {
            auto value = co_await source.next();
            if (!value) co_return;
            co_await shared->slots.acquire(1);
            if (shared->channel.is_closed()) {
                shared->slots.release(1);
                co_return;
            }
            Inner inner =
                co_await call_as_task<Inner>(shared->fn, std::move(*value));
            start_stream_pump(pump_stream<U>(shared, shared->channel.producer(),
                                             std::move(inner), &shared->slots),
                              shared->executor);
        }
    } catch (...) {
        error = std::current_exception();
    }
    StreamMessage<U> message;
    message.error = std::move(error);
    try {
        co_await shared->channel.send(std::move(message));
    } catch (...) {
        // A wait cancelled at shutdown. The reader sees the channel end.
    }
}

template <typename U>
AsyncGenerator<U> one_item(CoroTask<U> task) {
    U value = co_await std::move(task);
    co_yield std::move(value);
}

template <typename U>
AsyncGenerator<dstd::Pair<std::size_t, U>> one_indexed_item(std::size_t index,
                                                            CoroTask<U> task) {
    U value = co_await std::move(task);
    co_yield dstd::Pair<std::size_t, U>(index, std::move(value));
}

template <typename U, typename F>
struct OrderedShared : StreamShared<dstd::Pair<std::size_t, U>> {
    OrderedShared(std::size_t limit, Executor* exec, F function)
        : StreamShared<dstd::Pair<std::size_t, U>>(limit, exec),
          tokens(limit),
          fn(std::move(function)) {}
    AsyncSemaphore tokens;
    F fn;
};

template <typename S, typename U, typename F>
Coro pump_ordered(
    dstd::SharedPtr<OrderedShared<U, F>> shared,
    ChannelProducer<StreamMessage<dstd::Pair<std::size_t, U>>> producer,
    AsyncGenerator<S> source) {
    using Item = dstd::Pair<std::size_t, U>;
    source.stop_on(&shared->stopped);
    auto guard = producer.guard();
    std::exception_ptr error;
    try {
        for (std::size_t index = 0;; ++index) {
            auto value = co_await source.next();
            if (!value) co_return;
            co_await shared->tokens.acquire(1);
            if (shared->channel.is_closed()) co_return;
            start_stream_pump(
                pump_stream<Item>(
                    shared, shared->channel.producer(),
                    one_indexed_item<U>(
                        index, call_as_task<U>(shared->fn, std::move(*value)))),
                shared->executor);
        }
    } catch (...) {
        error = std::current_exception();
    }
    StreamMessage<Item> message;
    message.error = std::move(error);
    try {
        co_await shared->channel.send(std::move(message));
    } catch (...) {
        // A wait cancelled at shutdown. The reader sees the channel end.
    }
}

// A token is held from the start of a job until its result leaves the reader,
// which bounds the waiting results. The closer wakes a pump that waits for one.
template <typename U, typename F>
AsyncGenerator<U> read_ordered(dstd::SharedPtr<OrderedShared<U, F>> shared,
                               std::size_t limit) {
    struct Closer {
        OrderedShared<U, F>& shared;
        std::size_t limit;
        bool drained{false};
        ~Closer() {
            if (drained) return;
            shared.stop();
            shared.tokens.release(limit);
        }
    } closer{*shared, limit};
    dstd::Vector<dstd::Optional<U>> window(limit);
    std::size_t next = 0;
    for (;;) {
        auto message = co_await shared->channel.receive();
        if (!message) {
            closer.drained = true;
            co_return;
        }
        if (message->error) common::rethrow_and_clear(message->error);
        window[message->value->first % limit].emplace(
            std::move(message->value->second));
        for (;;) {
            auto& slot = window[next % limit];
            if (!slot) break;
            U item = std::move(*slot);
            slot.reset();
            ++next;
            shared->tokens.release(1);
            co_yield std::move(item);
        }
    }
}

template <typename T>
CoroTask<dstd::Optional<T>> next_item(AsyncGenerator<T>& source) {
    auto value = co_await source.next();
    co_return value;
}

}  // namespace detail

/**
 * @brief Yields `fn(item)` for each item, one at a time. `fn` returns a value
 * or a @ref CoroTask of a value.
 */
template <typename T, typename F, typename R = std::invoke_result_t<F&, T&&>,
          typename U = typename detail::UnwrapTask<R>::type>
    requires(!detail::is_async_generator_v<R> && !std::is_void_v<U>)
AsyncGenerator<U> map(AsyncGenerator<T> source, F fn) {
    for (;;) {
        auto value = co_await source.next();
        if (!value) break;
        if constexpr (detail::is_coro_task_v<R>) {
            U mapped = co_await std::invoke(fn, std::move(*value));
            co_yield std::move(mapped);
        } else {
            co_yield std::invoke(fn, std::move(*value));
        }
    }
}

/**
 * @brief Yields the items of the generator that `fn(item)` gives, for each item
 * in turn. `fn` returns a generator or a @ref CoroTask of one.
 */
template <typename T, typename F, typename R = std::invoke_result_t<F&, T&&>,
          typename Inner = typename detail::UnwrapTask<R>::type,
          typename U = typename Inner::value_type>
    requires detail::is_async_generator_v<Inner>
AsyncGenerator<U> flat_map(AsyncGenerator<T> source, F fn) {
    for (;;) {
        auto value = co_await source.next();
        if (!value) break;
        Inner inner =
            co_await detail::call_as_task<Inner>(fn, std::move(*value));
        for (;;) {
            auto item = co_await inner.next();
            if (!item) break;
            co_yield std::move(*item);
        }
    }
}

/// Yields the items for which @p pred holds. @p pred returns a bool or a
/// @ref CoroTask of one.
template <typename T, typename F>
AsyncGenerator<T> filter(AsyncGenerator<T> source, F pred) {
    using R = std::invoke_result_t<F&, const T&>;
    for (;;) {
        auto value = co_await source.next();
        if (!value) break;
        bool keep = false;
        if constexpr (detail::is_coro_task_v<R>) {
            keep = co_await std::invoke(pred, std::as_const(*value));
        } else {
            keep = std::invoke(pred, std::as_const(*value));
        }
        if (keep) co_yield std::move(*value);
    }
}

/// Yields at most @p count items, and does not read the source again after
/// them.
template <typename T>
AsyncGenerator<T> take(AsyncGenerator<T> source, std::size_t count) {
    for (std::size_t seen = 0; seen < count; ++seen) {
        auto value = co_await source.next();
        if (!value) break;
        co_yield std::move(*value);
    }
}

/// Yields items until @p pred holds. That item is not yielded, and the source
/// is not read again. @p pred returns a bool or a @ref CoroTask of one.
template <typename T, typename F>
AsyncGenerator<T> take_until(AsyncGenerator<T> source, F pred) {
    using R = std::invoke_result_t<F&, const T&>;
    for (;;) {
        auto value = co_await source.next();
        if (!value) co_return;
        bool stop = false;
        if constexpr (detail::is_coro_task_v<R>) {
            stop = co_await std::invoke(pred, std::as_const(*value));
        } else {
            stop = std::invoke(pred, std::as_const(*value));
        }
        if (stop) co_return;
        co_yield std::move(*value);
    }
}

/// Yields @p first and then @p second.
template <typename T>
AsyncGenerator<T> concat(AsyncGenerator<T> first, AsyncGenerator<T> second) {
    for (;;) {
        auto value = co_await first.next();
        if (!value) break;
        co_yield std::move(*value);
    }
    for (;;) {
        auto value = co_await second.next();
        if (!value) break;
        co_yield std::move(*value);
    }
}

/// Yields @p first and then the generator of `factory()`, which is called only
/// when @p first ends.
template <typename T, typename F>
    requires std::is_same_v<std::invoke_result_t<F&>, AsyncGenerator<T>>
AsyncGenerator<T> concat(AsyncGenerator<T> first, F factory) {
    for (;;) {
        auto value = co_await first.next();
        if (!value) break;
        co_yield std::move(*value);
    }
    auto second = std::invoke(factory);
    for (;;) {
        auto value = co_await second.next();
        if (!value) break;
        co_yield std::move(*value);
    }
}

/// Yields lists of @p size items, and a shorter last one with what is left, so
/// no item is lost. Raises an invalid-argument error on a size of 0.
template <typename T>
AsyncGenerator<dstd::Vector<T>> chunks(AsyncGenerator<T> source,
                                       std::size_t size) {
    if (size == 0) {
        throw common::DFTUtilsException(dstd::Error{
            dstd::Condition::InvalidArgument, "chunks needs a size above 0"});
    }
    dstd::Vector<T> chunk;
    chunk.reserve(size);
    for (;;) {
        auto value = co_await source.next();
        if (!value) break;
        chunk.push_back(std::move(*value));
        if (chunk.size() == size) {
            co_yield std::move(chunk);
            chunk = dstd::Vector<T>{};
            chunk.reserve(size);
        }
    }
    if (!chunk.empty()) co_yield std::move(chunk);
}

/// Yields pairs and ends with the shorter source. An item already read from the
/// longer source is dropped.
template <typename A, typename B>
AsyncGenerator<dstd::Pair<A, B>> zip(AsyncGenerator<A> first,
                                     AsyncGenerator<B> second) {
    for (;;) {
        auto a = co_await first.next();
        if (!a) co_return;
        auto b = co_await second.next();
        if (!b) co_return;
        co_yield dstd::Pair<A, B>(std::move(*a), std::move(*b));
    }
}

/// Folds the stream into one value. `fn(accumulator, item)` returns the next
/// accumulator or a @ref CoroTask of it.
template <typename T, typename Acc, typename F>
CoroTask<Acc> fold(AsyncGenerator<T> source, Acc init, F fn) {
    using R = std::invoke_result_t<F&, Acc&&, T&&>;
    Acc accumulator = std::move(init);
    for (;;) {
        auto value = co_await source.next();
        if (!value) break;
        if constexpr (detail::is_coro_task_v<R>) {
            Acc next = co_await std::invoke(fn, std::move(accumulator),
                                            std::move(*value));
            accumulator = std::move(next);
        } else {
            accumulator =
                std::invoke(fn, std::move(accumulator), std::move(*value));
        }
    }
    co_return accumulator;
}

/**
 * @brief Reads every source at once and yields items as they come.
 *
 * Items of one source keep their order. The first error is raised from the next
 * read. Dropping the generator ends the sources in the background at their
 * current wait, so a source must own what it uses.
 */
template <typename T>
AsyncGenerator<T> merge(dstd::Vector<AsyncGenerator<T>> sources) {
    if (sources.empty()) co_return;
    Executor* running = co_await this_coro::executor;
    Executor* executor = detail::require_stream_executor(running);
    auto shared =
        dstd::make_shared<detail::StreamShared<T>>(sources.size(), executor);
    for (auto& source : sources) {
        detail::start_stream_pump(
            detail::pump_stream<T>(shared, shared->channel.producer(),
                                   std::move(source)),
            executor);
    }
    auto reader = detail::read_stream<T>(std::move(shared));
    for (;;) {
        auto value = co_await reader.next();
        if (!value) co_return;
        co_yield std::move(*value);
    }
}

/**
 * @brief Reads @p source ahead of the reader, holding at most @p size items.
 *
 * Order holds. The source runs in the background and must own what it uses.
 * Raises an invalid-argument error on a size of 0.
 */
template <typename T>
AsyncGenerator<T> buffer(AsyncGenerator<T> source, std::size_t size) {
    if (size == 0) {
        throw common::DFTUtilsException(dstd::Error{
            dstd::Condition::InvalidArgument, "buffer needs a size above 0"});
    }
    Executor* running = co_await this_coro::executor;
    Executor* executor = detail::require_stream_executor(running);
    auto shared = dstd::make_shared<detail::StreamShared<T>>(size, executor);
    detail::start_stream_pump(
        detail::pump_stream<T>(shared, shared->channel.producer(),
                               std::move(source)),
        executor);
    auto reader = detail::read_stream<T>(std::move(shared));
    for (;;) {
        auto value = co_await reader.next();
        if (!value) co_return;
        co_yield std::move(*value);
    }
}

/**
 * @brief Runs `fn(item)`, which returns a generator or a @ref CoroTask of one,
 * for up to @p limit items at once, and yields their items as they come.
 *
 * Items of one inner generator keep their order. @p limit bounds the running
 * inner generators, and a new one starts when one ends. The first error is
 * raised from the next read. The generators run in the background and must own
 * what they use, and the stream keeps @p fn alive until its generators end.
 * Raises an invalid-argument error on a limit of 0.
 */
template <typename T, typename F, typename R = std::invoke_result_t<F&, T&&>,
          typename Inner = typename detail::UnwrapTask<R>::type,
          typename U = typename Inner::value_type>
AsyncGenerator<U> flat_map_merge(AsyncGenerator<T> source, F fn,
                                 std::size_t limit) {
    if (limit == 0) {
        throw common::DFTUtilsException(
            dstd::Error{dstd::Condition::InvalidArgument,
                        "flat_map_merge needs a limit above 0"});
    }
    Executor* running = co_await this_coro::executor;
    Executor* executor = detail::require_stream_executor(running);
    auto shared = dstd::make_shared<detail::MergeMapShared<U, F>>(
        limit, executor, std::move(fn));
    detail::start_stream_pump(
        detail::pump_outer<T, F, U>(shared, shared->channel.producer(),
                                    std::move(source)),
        executor);
    auto reader = detail::read_stream<U>(shared);
    for (;;) {
        auto value = co_await reader.next();
        if (!value) co_return;
        co_yield std::move(*value);
    }
}

/**
 * @brief Runs `fn(item)`, which gives a value or a @ref CoroTask of one, for up
 * to @p limit items at once, and yields the results as they finish.
 *
 * `fn` runs at the same time on separate items. It lives until the jobs end.
 * The first error is raised from the next read. Jobs run in the background and
 * must own what they use. Raises an invalid-argument error on a limit of 0.
 */
template <typename T, typename F,
          typename U =
              typename detail::UnwrapTask<std::invoke_result_t<F&, T&&>>::type>
AsyncGenerator<U> map_unordered(AsyncGenerator<T> source, F fn,
                                std::size_t limit) {
    auto inner = [fn = std::move(fn)](T value) mutable {
        return detail::one_item<U>(
            detail::call_as_task<U>(fn, std::move(value)));
    };
    return flat_map_merge(std::move(source), std::move(inner), limit);
}

/**
 * @brief @ref map_unordered that yields results in the order of @p source.
 *
 * @p limit tokens bound the jobs that run plus the results that wait for an
 * earlier one, so a slow item holds back at most that many. Raises an
 * invalid-argument error on a limit of 0.
 */
template <typename T, typename F,
          typename U =
              typename detail::UnwrapTask<std::invoke_result_t<F&, T&&>>::type>
AsyncGenerator<U> map_ordered(AsyncGenerator<T> source, F fn,
                              std::size_t limit) {
    if (limit == 0) {
        throw common::DFTUtilsException(
            dstd::Error{dstd::Condition::InvalidArgument,
                        "map_ordered needs a limit above 0"});
    }
    Executor* running = co_await this_coro::executor;
    Executor* executor = detail::require_stream_executor(running);
    auto shared = dstd::make_shared<detail::OrderedShared<U, F>>(
        limit, executor, std::move(fn));
    detail::start_stream_pump(
        detail::pump_ordered<T, U, F>(shared, shared->channel.producer(),
                                      std::move(source)),
        executor);
    auto reader = detail::read_ordered<U, F>(shared, limit);
    for (;;) {
        auto value = co_await reader.next();
        if (!value) co_return;
        co_yield std::move(*value);
    }
}

/**
 * @brief Yields the items of @p source, and raises a dstd::Condition::Timeout
 * error when one takes longer than @p wait to come.
 *
 * The wait in the source ends with the timeout. A source that computes without
 * waiting delays the error until its next wait.
 */
template <typename T, typename Rep, typename Period>
AsyncGenerator<T> with_item_timeout(AsyncGenerator<T> source,
                                    std::chrono::duration<Rep, Period> wait) {
    for (;;) {
        auto value = co_await with_timeout(detail::next_item<T>(source), wait);
        if (!value) co_return;
        co_yield std::move(*value);
    }
}

/// @ref map on a sync @ref Generator. `fn` gives a value.
template <typename T, typename F, typename R = std::invoke_result_t<F&, T&&>>
    requires(!detail::is_generator_v<R> && !detail::is_coro_task_v<R> &&
             !std::is_void_v<R>)
Generator<R> map(Generator<T> source, F fn) {
    for (;;) {
        auto value = source.next();
        if (!value) break;
        co_yield std::invoke(fn, std::move(*value));
    }
}

/// @ref flat_map on a sync @ref Generator. `fn` gives a @ref Generator.
template <typename T, typename F,
          typename Inner = std::invoke_result_t<F&, T&&>,
          typename U = typename Inner::value_type>
    requires detail::is_generator_v<Inner>
Generator<U> flat_map(Generator<T> source, F fn) {
    for (;;) {
        auto value = source.next();
        if (!value) break;
        Inner inner = std::invoke(fn, std::move(*value));
        for (;;) {
            auto item = inner.next();
            if (!item) break;
            co_yield std::move(*item);
        }
    }
}

/// @ref filter on a sync @ref Generator.
template <typename T, typename F>
Generator<T> filter(Generator<T> source, F pred) {
    for (;;) {
        auto value = source.next();
        if (!value) break;
        if (std::invoke(pred, std::as_const(*value))) {
            co_yield std::move(*value);
        }
    }
}

/// @ref take on a sync @ref Generator.
template <typename T>
Generator<T> take(Generator<T> source, std::size_t count) {
    for (std::size_t seen = 0; seen < count; ++seen) {
        auto value = source.next();
        if (!value) break;
        co_yield std::move(*value);
    }
}

/// @ref take_until on a sync @ref Generator.
template <typename T, typename F>
Generator<T> take_until(Generator<T> source, F pred) {
    for (;;) {
        auto value = source.next();
        if (!value) co_return;
        if (std::invoke(pred, std::as_const(*value))) co_return;
        co_yield std::move(*value);
    }
}

/// @ref concat on a sync @ref Generator.
template <typename T>
Generator<T> concat(Generator<T> first, Generator<T> second) {
    for (;;) {
        auto value = first.next();
        if (!value) break;
        co_yield std::move(*value);
    }
    for (;;) {
        auto value = second.next();
        if (!value) break;
        co_yield std::move(*value);
    }
}

/// @ref concat with a factory on a sync @ref Generator.
template <typename T, typename F>
    requires std::is_same_v<std::invoke_result_t<F&>, Generator<T>>
Generator<T> concat(Generator<T> first, F factory) {
    for (;;) {
        auto value = first.next();
        if (!value) break;
        co_yield std::move(*value);
    }
    auto second = std::invoke(factory);
    for (;;) {
        auto value = second.next();
        if (!value) break;
        co_yield std::move(*value);
    }
}

/// @ref chunks on a sync @ref Generator.
template <typename T>
Generator<dstd::Vector<T>> chunks(Generator<T> source, std::size_t size) {
    if (size == 0) {
        throw common::DFTUtilsException(dstd::Error{
            dstd::Condition::InvalidArgument, "chunks needs a size above 0"});
    }
    dstd::Vector<T> chunk;
    chunk.reserve(size);
    for (;;) {
        auto value = source.next();
        if (!value) break;
        chunk.push_back(std::move(*value));
        if (chunk.size() == size) {
            co_yield std::move(chunk);
            chunk = dstd::Vector<T>{};
            chunk.reserve(size);
        }
    }
    if (!chunk.empty()) co_yield std::move(chunk);
}

/// @ref zip on a sync @ref Generator.
template <typename A, typename B>
Generator<dstd::Pair<A, B>> zip(Generator<A> first, Generator<B> second) {
    for (;;) {
        auto a = first.next();
        if (!a) co_return;
        auto b = second.next();
        if (!b) co_return;
        co_yield dstd::Pair<A, B>(std::move(*a), std::move(*b));
    }
}

/// @ref fold on a sync @ref Generator. Gives the value at once.
template <typename T, typename Acc, typename F>
Acc fold(Generator<T> source, Acc init, F fn) {
    Acc accumulator = std::move(init);
    for (;;) {
        auto value = source.next();
        if (!value) break;
        accumulator =
            std::invoke(fn, std::move(accumulator), std::move(*value));
    }
    return accumulator;
}

/**
 * @brief The items of a sync @ref Generator as an @ref AsyncGenerator, so the
 * operations that need an executor apply.
 *
 * The body runs on the thread that reads, and it cannot wait, so a slow step
 * holds that thread and a timeout fires only after the step ends.
 */
template <typename T>
AsyncGenerator<T> to_async(Generator<T> source) {
    for (;;) {
        auto value = source.next();
        if (!value) break;
        co_yield std::move(*value);
    }
}

namespace detail {

template <typename T>
inline constexpr bool NEEDS_TO_ASYNC_V = false;

}  // namespace detail

template <typename T>
void merge(dstd::Vector<Generator<T>>) {
    static_assert(detail::NEEDS_TO_ASYNC_V<T>,
                  "merge needs an AsyncGenerator: use coro::to_async");
}

template <typename T, typename... Args>
void buffer(Generator<T>, Args&&...) {
    static_assert(detail::NEEDS_TO_ASYNC_V<T>,
                  "buffer needs an AsyncGenerator: use coro::to_async");
}

template <typename T, typename... Args>
void flat_map_merge(Generator<T>, Args&&...) {
    static_assert(detail::NEEDS_TO_ASYNC_V<T>,
                  "flat_map_merge needs an AsyncGenerator: use coro::to_async");
}

template <typename T, typename... Args>
void map_ordered(Generator<T>, Args&&...) {
    static_assert(detail::NEEDS_TO_ASYNC_V<T>,
                  "map_ordered needs an AsyncGenerator: use coro::to_async");
}

template <typename T, typename... Args>
void map_unordered(Generator<T>, Args&&...) {
    static_assert(detail::NEEDS_TO_ASYNC_V<T>,
                  "map_unordered needs an AsyncGenerator: use coro::to_async");
}

template <typename T, typename... Args>
void with_item_timeout(Generator<T>, Args&&...) {
    static_assert(detail::NEEDS_TO_ASYNC_V<T>,
                  "with_item_timeout needs an AsyncGenerator: use "
                  "coro::to_async");
}

/// @ref map as an adaptor.
template <typename F>
auto map(F fn) {
    return detail::make_adaptor([fn = std::move(fn)](auto&& source) {
        return coro::map(std::forward<decltype(source)>(source), fn);
    });
}

/// @ref flat_map as an adaptor.
template <typename F>
auto flat_map(F fn) {
    return detail::make_adaptor([fn = std::move(fn)](auto&& source) {
        return coro::flat_map(std::forward<decltype(source)>(source), fn);
    });
}

/// @ref filter as an adaptor.
template <typename F>
auto filter(F pred) {
    return detail::make_adaptor([pred = std::move(pred)](auto&& source) {
        return coro::filter(std::forward<decltype(source)>(source), pred);
    });
}

/// @ref take as an adaptor.
inline auto take(std::size_t count) {
    return detail::make_adaptor([count](auto&& source) {
        return coro::take(std::forward<decltype(source)>(source), count);
    });
}

/// @ref take_until as an adaptor.
template <typename F>
auto take_until(F pred) {
    return detail::make_adaptor([pred = std::move(pred)](auto&& source) {
        return coro::take_until(std::forward<decltype(source)>(source), pred);
    });
}

/// @ref concat with a factory, as an adaptor.
template <typename F>
auto concat(F factory) {
    return detail::make_adaptor([factory = std::move(factory)](auto&& source) {
        return coro::concat(std::forward<decltype(source)>(source), factory);
    });
}

/// @ref chunks as an adaptor.
inline auto chunks(std::size_t size) {
    return detail::make_adaptor([size](auto&& source) {
        return coro::chunks(std::forward<decltype(source)>(source), size);
    });
}

/// @ref buffer as an adaptor.
inline auto buffer(std::size_t size) {
    return detail::make_adaptor([size](auto&& source) {
        return coro::buffer(std::forward<decltype(source)>(source), size);
    });
}

/// @ref fold as an adaptor.
template <typename Acc, typename F>
auto fold(Acc init, F fn) {
    return detail::make_adaptor(
        [init = std::move(init), fn = std::move(fn)](auto&& source) {
            return coro::fold(std::forward<decltype(source)>(source), init, fn);
        });
}

/// @ref map_ordered as an adaptor.
template <typename F>
auto map_ordered(F fn, std::size_t limit) {
    return detail::make_adaptor([fn = std::move(fn), limit](auto&& source) {
        return coro::map_ordered(std::forward<decltype(source)>(source), fn,
                                 limit);
    });
}

/// @ref map_unordered as an adaptor.
template <typename F>
auto map_unordered(F fn, std::size_t limit) {
    return detail::make_adaptor([fn = std::move(fn), limit](auto&& source) {
        return coro::map_unordered(std::forward<decltype(source)>(source), fn,
                                   limit);
    });
}

/// @ref flat_map_merge as an adaptor.
template <typename F>
auto flat_map_merge(F fn, std::size_t limit) {
    return detail::make_adaptor([fn = std::move(fn), limit](auto&& source) {
        return coro::flat_map_merge(std::forward<decltype(source)>(source), fn,
                                    limit);
    });
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_STREAM_H
