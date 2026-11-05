// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_STREAM_H
#define DFTRACER_UTILS_CORO_STREAM_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
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
// because the generator can be dropped while they run. They end at their next
// send once the generator closes the channel.
template <typename T>
struct StreamShared {
    explicit StreamShared(std::size_t capacity) : channel(capacity) {}
    Channel<StreamMessage<T>> channel;
};

template <typename T>
struct StreamCloser {
    Channel<StreamMessage<T>>& channel;
    ~StreamCloser() { channel.close(); }
};

inline void start_stream_pump(Coro pump, Executor* executor) {
    pump.handle().promise().executor = executor;
    executor->post(pump.release());
}

inline Executor* stream_executor() {
    Executor* executor = Executor::current();
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
CoroTask<dstd::Optional<StreamMessage<T>>> receive_message(
    Channel<StreamMessage<T>>& channel) {
    co_return co_await channel.receive();
}

template <typename T>
AsyncGenerator<T> read_stream(dstd::SharedPtr<StreamShared<T>> shared) {
    StreamCloser<T> closer{shared->channel};
    for (;;) {
        auto message = co_await shared->channel.receive();
        if (!message) co_return;
        if (message->error) common::rethrow_and_clear(message->error);
        co_yield std::move(*message->value);
    }
}

// Holds fn because a coroutine lambda reads its captures through the closure,
// and its generators outlive the outer pump.
template <typename T, typename F>
struct MergeMapShared : StreamShared<T> {
    MergeMapShared(std::size_t limit, F function)
        : StreamShared<T>(limit), slots(limit), fn(std::move(function)) {}
    AsyncSemaphore slots;
    F fn;
};

template <typename S, typename F, typename U>
Coro pump_outer(dstd::SharedPtr<MergeMapShared<U, F>> shared,
                ChannelProducer<StreamMessage<U>> producer,
                AsyncGenerator<S> source, Executor* executor) {
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
            start_stream_pump(
                pump_stream<U>(shared, shared->channel.producer(),
                               std::invoke(shared->fn, std::move(*value)),
                               &shared->slots),
                executor);
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

}  // namespace detail

/// Yields items until @p pred holds. That item is not yielded, and the source
/// is not read again.
template <typename T, typename F>
AsyncGenerator<T> take_until(AsyncGenerator<T> source, F pred) {
    for (;;) {
        auto value = co_await source.next();
        if (!value) co_return;
        if (std::invoke(pred, *value)) co_return;
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
/// accumulator.
template <typename T, typename Acc, typename F>
CoroTask<Acc> fold(AsyncGenerator<T> source, Acc init, F fn) {
    Acc accumulator = std::move(init);
    for (;;) {
        auto value = co_await source.next();
        if (!value) break;
        accumulator =
            std::invoke(fn, std::move(accumulator), std::move(*value));
    }
    co_return accumulator;
}

/**
 * @brief Reads every source at once and yields items as they come.
 *
 * Items of one source keep their order. The first error is raised from the next
 * read. Dropping the generator ends the sources in the background, at their
 * next send, so a source must own what it uses.
 */
template <typename T>
AsyncGenerator<T> merge(dstd::Vector<AsyncGenerator<T>> sources) {
    if (sources.empty()) co_return;
    Executor* executor = detail::stream_executor();
    auto shared = dstd::make_shared<detail::StreamShared<T>>(sources.size());
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
    Executor* executor = detail::stream_executor();
    auto shared = dstd::make_shared<detail::StreamShared<T>>(size);
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
 * @brief Runs `fn(item)`, which returns a generator, for up to @p limit items
 * at once, and yields their items as they come.
 *
 * Items of one inner generator keep their order. @p limit bounds the running
 * inner generators, and a new one starts when one ends. The first error is
 * raised from the next read. The generators run in the background and must own
 * what they use, and the stream keeps @p fn alive until its generators end.
 * Raises an invalid-argument error on a limit of 0.
 */
template <typename T, typename F,
          typename Inner = std::invoke_result_t<F&, T&&>,
          typename U = typename Inner::value_type>
AsyncGenerator<U> flat_map_merge(AsyncGenerator<T> source, F fn,
                                 std::size_t limit) {
    if (limit == 0) {
        throw common::DFTUtilsException(
            dstd::Error{dstd::Condition::InvalidArgument,
                        "flat_map_merge needs a limit above 0"});
    }
    Executor* executor = detail::stream_executor();
    auto shared =
        dstd::make_shared<detail::MergeMapShared<U, F>>(limit, std::move(fn));
    detail::start_stream_pump(
        detail::pump_outer<T, F, U>(shared, shared->channel.producer(),
                                    std::move(source), executor),
        executor);
    auto reader = detail::read_stream<U>(shared);
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
 * The source runs in the background and reads one item ahead, because a wait
 * inside a generator cannot be cancelled from outside. So it must own what it
 * uses, and after a timeout it stops only when its own wait ends.
 */
template <typename T, typename Rep, typename Period>
AsyncGenerator<T> with_item_timeout(AsyncGenerator<T> source,
                                    std::chrono::duration<Rep, Period> wait) {
    Executor* executor = detail::stream_executor();
    auto shared = dstd::make_shared<detail::StreamShared<T>>(1);
    detail::start_stream_pump(
        detail::pump_stream<T>(shared, shared->channel.producer(),
                               std::move(source)),
        executor);
    detail::StreamCloser<T> closer{shared->channel};
    for (;;) {
        auto message = co_await with_timeout(
            detail::receive_message<T>(shared->channel), wait);
        if (!message) co_return;
        if (message->error) common::rethrow_and_clear(message->error);
        co_yield std::move(*message->value);
    }
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_STREAM_H
