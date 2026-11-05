// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using coro::detail::PopStatus;
using coro::detail::PushStatus;
using coro::detail::RingQueue;
using coro::detail::UnboundedQueue;

namespace {

struct Tagged {
    std::uint32_t producer = 0;
    std::uint32_t index = 0;
};

// Not default-constructible, and counts how many exist.
struct Counted {
    explicit Counted(std::atomic<int>& live_count) : live(&live_count) {
        ++*live;
    }
    Counted(Counted&& other) noexcept : live(other.live) { ++*live; }
    Counted(const Counted&) = delete;
    Counted& operator=(const Counted&) = delete;
    ~Counted() { --*live; }
    std::atomic<int>* live;
};

template <typename Queue>
void push_all_until_closed(Queue& queue, std::uint64_t first,
                           std::atomic<std::uint64_t>& pushed_sum,
                           std::atomic<std::uint64_t>& pushed_count) {
    std::uint64_t value = first;
    for (;;) {
        std::uint64_t item = value;
        const PushStatus status = queue.try_push(item);
        if (status == PushStatus::Closed) return;
        if (status == PushStatus::Full) continue;
        pushed_sum.fetch_add(value);
        pushed_count.fetch_add(1);
        ++value;
    }
}

template <typename Queue>
void pop_all_until_closed(Queue& queue, std::atomic<std::uint64_t>& popped_sum,
                          std::atomic<std::uint64_t>& popped_count) {
    for (;;) {
        dstd::Optional<std::uint64_t> item;
        const PopStatus status = queue.try_pop(item);
        if (status == PopStatus::Closed) return;
        if (status == PopStatus::Empty) continue;
        popped_sum.fetch_add(*item);
        popped_count.fetch_add(1);
    }
}

// Producers run until the close, so a push racing the close must either fail
// or reach a consumer.
template <typename Queue>
void check_close_race(Queue& queue) {
    std::atomic<std::uint64_t> pushed_sum{0};
    std::atomic<std::uint64_t> pushed_count{0};
    std::atomic<std::uint64_t> popped_sum{0};
    std::atomic<std::uint64_t> popped_count{0};
    dstd::Vector<dstd::Thread> threads;
    for (std::uint64_t p = 0; p < 3; ++p) {
        threads.emplace_back([&, p] {
            push_all_until_closed(queue, (p + 1) << 40, pushed_sum,
                                  pushed_count);
        });
    }
    for (int c = 0; c < 3; ++c) {
        threads.emplace_back(
            [&] { pop_all_until_closed(queue, popped_sum, popped_count); });
    }
    while (pushed_count.load() < 2000) std::this_thread::yield();
    queue.mark_closed();
    for (auto& thread : threads) thread.join();
    CHECK(popped_count.load() == pushed_count.load());
    CHECK(popped_sum.load() == pushed_sum.load());
}

CoroTask<void> send_tagged(Channel<Tagged>& channel,
                           ChannelProducer<Tagged> producer,
                           std::uint32_t producer_id, std::uint32_t count) {
    auto guard = producer.guard();
    for (std::uint32_t i = 0; i < count; ++i) {
        Tagged item{producer_id, i};
        const bool sent = co_await channel.send(std::move(item));
        CHECK(sent);
    }
}

// One consumer, so what it sees of each producer must stay in order.
CoroTask<void> receive_in_order(Channel<Tagged>& channel,
                                dstd::Vector<std::uint32_t>& next,
                                std::uint64_t& seen) {
    for (;;) {
        auto item = co_await channel.receive();
        if (!item) break;
        CHECK(item->index == next[item->producer]);
        next[item->producer] = item->index + 1;
        ++seen;
    }
}

template <typename ChannelT>
CoroTask<void> receive_expect_end(ChannelT& channel, std::atomic<int>& ended) {
    auto item = co_await channel.receive();
    CHECK(!item);
    ended.fetch_add(1);
}

CoroTask<void> send_expect_refused(Channel<int>& channel,
                                   std::atomic<int>& refused,
                                   std::atomic<int>& parked) {
    parked.fetch_add(1);
    const bool sent = co_await channel.send(7);
    if (!sent) refused.fetch_add(1);
}

CoroTask<void> send_until_closed(Channel<std::uint64_t>& channel,
                                 std::uint64_t first,
                                 std::atomic<std::uint64_t>& sent_sum,
                                 std::atomic<std::uint64_t>& sent_count) {
    for (std::uint64_t value = first;; ++value) {
        const bool sent = co_await channel.send(std::uint64_t{value});
        if (!sent) co_return;
        sent_sum.fetch_add(value);
        sent_count.fetch_add(1);
    }
}

CoroTask<void> receive_sum(Channel<std::uint64_t>& channel,
                           std::atomic<std::uint64_t>& sum,
                           std::atomic<std::uint64_t>& count) {
    for (;;) {
        auto item = co_await channel.receive();
        if (!item) co_return;
        sum.fetch_add(*item);
        count.fetch_add(1);
    }
}

CoroTask<void> close_after(Channel<std::uint64_t>& channel,
                           std::atomic<std::uint64_t>& sent_count,
                           std::uint64_t at_least) {
    while (sent_count.load() < at_least) {
        co_await yield();
    }
    channel.close();
}

}  // namespace

TEST_CASE("RingQueue holds exactly its capacity for every lap") {
    for (std::size_t capacity : {1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 64u}) {
        CAPTURE(capacity);
        RingQueue<int> queue(capacity);
        CHECK(queue.capacity() == capacity);
        int next_push = 0;
        int next_pop = 0;
        for (int lap = 0; lap < 5; ++lap) {
            for (std::size_t i = 0; i < capacity; ++i) {
                int item = next_push++;
                REQUIRE(queue.try_push(item) == PushStatus::Ok);
            }
            CHECK(queue.size() == capacity);
            int refused = -1;
            CHECK(queue.try_push(refused) == PushStatus::Full);
            CHECK(refused == -1);
            for (std::size_t i = 0; i < capacity; ++i) {
                dstd::Optional<int> item;
                REQUIRE(queue.try_pop(item) == PopStatus::Ok);
                CHECK(*item == next_pop++);
            }
            dstd::Optional<int> none;
            CHECK(queue.try_pop(none) == PopStatus::Empty);
            CHECK(!none);
            CHECK(queue.size() == 0);
        }
    }
}

TEST_CASE("RingQueue moves an item only when the push succeeds") {
    RingQueue<dstd::UniquePtr<int>> queue(1);
    auto first = dstd::make_unique<int>(1);
    auto second = dstd::make_unique<int>(2);
    CHECK(queue.try_push(first) == PushStatus::Ok);
    CHECK(!first);
    CHECK(queue.try_push(second) == PushStatus::Full);
    REQUIRE(second);
    queue.mark_closed();
    CHECK(queue.try_push(second) == PushStatus::Closed);
    REQUIRE(second);
    CHECK(*second == 2);
}

TEST_CASE("RingQueue keeps items readable after close, then reports closed") {
    RingQueue<int> queue(4);
    int a = 1;
    int b = 2;
    queue.try_push(a);
    queue.try_push(b);
    queue.mark_closed();
    dstd::Optional<int> out;
    REQUIRE(queue.try_pop(out) == PopStatus::Ok);
    CHECK(*out == 1);
    out.reset();
    REQUIRE(queue.try_pop(out) == PopStatus::Ok);
    CHECK(*out == 2);
    out.reset();
    CHECK(queue.try_pop(out) == PopStatus::Closed);
    CHECK(queue.size() == 0);
}

TEST_CASE("RingQueue takes a type with no default and destroys what is left") {
    std::atomic<int> live{0};
    {
        RingQueue<Counted> queue(4);
        for (int i = 0; i < 3; ++i) {
            Counted item(live);
            REQUIRE(queue.try_push(item) == PushStatus::Ok);
        }
        dstd::Optional<Counted> out;
        REQUIRE(queue.try_pop(out) == PopStatus::Ok);
        out.reset();
        CHECK(live.load() == 2);
    }
    CHECK(live.load() == 0);
}

TEST_CASE("RingQueue loses and duplicates nothing when a close races pushes") {
    for (std::size_t capacity : {1u, 3u, 16u}) {
        CAPTURE(capacity);
        RingQueue<std::uint64_t> queue(capacity);
        check_close_race(queue);
    }
}

TEST_CASE("UnboundedQueue loses and duplicates nothing when a close races") {
    UnboundedQueue<std::uint64_t> queue(16);
    check_close_race(queue);
}

TEST_CASE("Channel keeps the order of each producer past parked senders") {
    constexpr std::uint32_t PRODUCERS = 4;
    constexpr std::uint32_t PER_PRODUCER = 5000;
    for (std::size_t capacity : {1u, 2u, 7u}) {
        CAPTURE(capacity);
        Runtime runtime(4);
        dstd::Vector<std::uint32_t> next(PRODUCERS, 0);
        std::uint64_t seen = 0;
        runtime.block_on([&]() -> CoroTask<void> {
            Channel<Tagged> channel(capacity);
            Scope scope;
            for (std::uint32_t p = 0; p < PRODUCERS; ++p) {
                scope.spawn(
                    send_tagged(channel, channel.producer(), p, PER_PRODUCER));
            }
            scope.spawn(receive_in_order(channel, next, seen));
            co_await scope.join();
        }());
        CHECK(seen == PRODUCERS * PER_PRODUCER);
    }
}

TEST_CASE("Channel close wakes more parked coroutines than one batch holds") {
    constexpr int WAITERS = 64;
    Runtime runtime(2);

    std::atomic<int> ended{0};
    runtime.block_on([&]() -> CoroTask<void> {
        Channel<int> channel(4);
        Scope scope;
        for (int i = 0; i < WAITERS; ++i) {
            scope.spawn(receive_expect_end(channel, ended));
        }
        co_await yield();
        channel.close();
        co_await scope.join();
    }());
    CHECK(ended.load() == WAITERS);

    std::atomic<int> refused{0};
    std::atomic<int> parked{0};
    runtime.block_on([&]() -> CoroTask<void> {
        Channel<int> channel(1);
        CHECK(channel.try_send(1));
        Scope scope;
        for (int i = 0; i < WAITERS; ++i) {
            scope.spawn(send_expect_refused(channel, refused, parked));
        }
        while (parked.load() < WAITERS) co_await yield();
        channel.close();
        co_await scope.join();
        int left = 0;
        CHECK(channel.try_receive(left));
        CHECK(left == 1);
    }());
    CHECK(refused.load() == WAITERS);
}

TEST_CASE("Channel delivers every send that returned true when closed midway") {
    for (std::size_t capacity : {1u, 8u}) {
        CAPTURE(capacity);
        Runtime runtime(4);
        std::atomic<std::uint64_t> sent_sum{0};
        std::atomic<std::uint64_t> sent_count{0};
        std::atomic<std::uint64_t> received_sum{0};
        std::atomic<std::uint64_t> received_count{0};
        runtime.block_on([&]() -> CoroTask<void> {
            Channel<std::uint64_t> channel(capacity);
            Scope scope;
            for (std::uint64_t p = 0; p < 4; ++p) {
                scope.spawn(send_until_closed(channel, (p + 1) << 40, sent_sum,
                                              sent_count));
            }
            for (int r = 0; r < 4; ++r) {
                scope.spawn(receive_sum(channel, received_sum, received_count));
            }
            scope.spawn(close_after(channel, sent_count, 2000));
            co_await scope.join();
        }());
        CHECK(received_count.load() == sent_count.load());
        CHECK(received_sum.load() == sent_sum.load());
    }
}

TEST_CASE("Channel blocking_receive sees every item, then the end") {
    constexpr std::uint32_t PER_PRODUCER = 3000;
    Runtime runtime(2);
    auto channel = make_channel<Tagged>(2);
    dstd::Vector<std::uint32_t> next(2, 0);
    std::uint64_t seen = 0;
    dstd::Thread reader([&] {
        while (auto item = channel->blocking_receive()) {
            CHECK(item->index == next[item->producer]);
            next[item->producer] = item->index + 1;
            ++seen;
        }
    });
    runtime.block_on([&]() -> CoroTask<void> {
        Scope scope;
        for (std::uint32_t p = 0; p < 2; ++p) {
            scope.spawn(
                send_tagged(*channel, channel->producer(), p, PER_PRODUCER));
        }
        co_await scope.join();
    }());
    reader.join();
    CHECK(seen == 2 * PER_PRODUCER);
}

TEST_CASE("UnboundedChannel never refuses and ends after the last producer") {
    Runtime runtime(2);
    auto channel = make_unbounded_channel<std::uint64_t>();
    CHECK(channel->capacity() == SIZE_MAX);
    std::uint64_t sum = 0;
    std::uint64_t count = 0;
    runtime.block_on([&]() -> CoroTask<void> {
        auto producer = channel->producer();
        {
            auto guard = producer.guard();
            for (std::uint64_t i = 1; i <= 10000; ++i) {
                const bool sent = co_await channel->send(std::uint64_t{i});
                CHECK(sent);
            }
            CHECK(channel->size() == 10000);
        }
        for (;;) {
            auto item = co_await channel->receive();
            if (!item) break;
            sum += *item;
            ++count;
        }
    }());
    CHECK(count == 10000);
    CHECK(sum == 10000 * 10001 / 2);
    CHECK(channel->is_closed_and_done());
}

TEST_CASE("channel zero capacity child" * doctest::skip()) {
    fresh_process::announce();
    Channel<int> channel(0);
}

TEST_CASE("Channel with capacity 0 aborts with a message") {
    auto result = fresh_process::run("channel zero capacity child");
    CHECK(result.announced);
    CHECK(WIFSIGNALED(result.status));
    CHECK(result.output.find("at least 1") != result.output.npos);
}
