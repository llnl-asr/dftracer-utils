// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/generator.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;

namespace {

#if defined(__SANITIZE_THREAD__)
constexpr bool TSAN_BUILD = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool TSAN_BUILD = true;
#else
constexpr bool TSAN_BUILD = false;
#endif
#else
constexpr bool TSAN_BUILD = false;
#endif

template <typename ChannelT, typename ItemT>
bool blocking_send(ChannelT& channel, ItemT item) {
    constexpr auto backoff = std::chrono::microseconds{100};
    while (true) {
        if (channel.try_send(item)) {
            return true;
        }
        if (channel.is_closed()) {
            return false;
        }
        std::this_thread::sleep_for(backoff);
    }
}

template <typename ChannelT, typename ItemT>
bool blocking_receive(ChannelT& channel, ItemT& item) {
    constexpr auto backoff = std::chrono::microseconds{100};
    while (true) {
        if (channel.try_receive(item)) {
            return true;
        }
        if (channel.is_closed_and_done()) {
            return false;
        }
        std::this_thread::sleep_for(backoff);
    }
}

void run_together(Runtime& rt, dstd::Vector<coro::CoroTask<void>> tasks) {
    rt.block_on(
        coro::with_scope([&tasks](coro::Scope& scope) -> coro::CoroTask<void> {
            for (auto& task : tasks) scope.spawn(std::move(task));
            co_return;
        }));
}

}  // namespace

// ============================================================================
// Basic Channel Tests
// ============================================================================

TEST_CASE("Channel - Basic construction") {
    Channel<int> channel(100);

    CHECK(channel.capacity() == 100);
    CHECK(channel.size() == 0);
    CHECK(channel.empty() == true);
    CHECK(channel.is_closed() == false);
}

TEST_CASE("Channel - Send and receive" * doctest::test_suite("vg")) {
    Channel<int> channel(10);

    // Send items
    CHECK(channel.try_send(42) == true);
    CHECK(channel.try_send(100) == true);
    CHECK(channel.size() == 2);

    // Receive items
    int value;
    CHECK(blocking_receive(channel, value) == true);
    CHECK(value == 42);

    CHECK(blocking_receive(channel, value) == true);
    CHECK(value == 100);

    CHECK(channel.empty() == true);
}

TEST_CASE("Channel - Try send and try receive") {
    Channel<int> channel(2);

    // Try send until full
    CHECK(channel.try_send(1) == true);
    CHECK(channel.try_send(2) == true);
    CHECK(channel.full() == true);

    // Try receive all
    int value;
    CHECK(channel.try_receive(value) == true);
    CHECK(value == 1);

    CHECK(channel.try_receive(value) == true);
    CHECK(value == 2);

    // Try receive from empty
    CHECK(channel.try_receive(value) == false);
}

TEST_CASE("Channel - Close channel") {
    Channel<int> channel(10);

    CHECK(channel.try_send(42));
    channel.close();

    CHECK(channel.is_closed() == true);

    // Can still receive existing items
    int value;
    CHECK(blocking_receive(channel, value) == true);
    CHECK(value == 42);

    // Cannot send after close
    CHECK(channel.try_send(100) == false);
    CHECK(channel.try_send(100) == false);
}

TEST_CASE("Channel - Producer guard") {
    Channel<int> channel(10);

    CHECK(channel.producer_count() == 0);

    {
        auto p1 = channel.producer();
        auto guard1 = p1.guard();
        CHECK(channel.producer_count() == 1);

        {
            auto p2 = channel.producer();
            auto guard2 = p2.guard();
            CHECK(channel.producer_count() == 2);
        }

        CHECK(channel.producer_count() == 1);
    }

    CHECK(channel.producer_count() == 0);
    CHECK(channel.is_closed() == true);
}

// ============================================================================
// Threaded Producer-Consumer Tests
// ============================================================================

TEST_CASE("Channel - Single producer, single consumer") {
    Channel<int> channel(100);

    constexpr int NUM_ITEMS = 1000;
    std::atomic<int> sum_produced{0};
    std::atomic<int> sum_consumed{0};

    // Producer thread
    dstd::Thread producer([&, p = channel.producer()]() mutable {
        auto guard = p.guard();
        for (int i = 0; i < NUM_ITEMS; ++i) {
            if (blocking_send(channel, i)) sum_produced.fetch_add(i);
        }
    });

    // Consumer thread
    dstd::Thread consumer([&]() {
        int value;
        while (blocking_receive(channel, value)) {
            sum_consumed.fetch_add(value);
        }
    });

    producer.join();
    consumer.join();

    CHECK(sum_produced.load() == sum_consumed.load());
}

TEST_CASE("Channel - receive waits before first producer registration") {
    Channel<int> channel(10);

    std::atomic<bool> consumer_entered{false};
    std::atomic<bool> consumer_done{false};
    std::atomic<bool> receive_success{false};
    std::atomic<int> received_value{-1};

    dstd::Thread consumer([&]() {
        int value = 0;
        consumer_entered.store(true, std::memory_order_release);
        bool success = blocking_receive(channel, value);
        receive_success.store(success, std::memory_order_release);
        received_value.store(value, std::memory_order_release);
        consumer_done.store(true, std::memory_order_release);
    });

    while (!consumer_entered.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    CHECK(consumer_done.load(std::memory_order_acquire) == false);

    dstd::Thread producer([&, p = channel.producer()]() mutable {
        auto guard = p.guard();
        CHECK(channel.producer_count() >= 1);
        CHECK(blocking_send(channel, 123));
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!consumer_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    const bool completed = consumer_done.load(std::memory_order_acquire);
    if (!completed) {
        channel.close();
    }

    producer.join();
    consumer.join();

    CHECK(completed == true);
    CHECK(receive_success.load(std::memory_order_acquire) == true);
    CHECK(received_value.load(std::memory_order_acquire) == 123);
}

TEST_CASE("Channel - Multiple producers, single consumer" *
          doctest::test_suite("vg")) {
    Channel<int> channel(100);

    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 250;
    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};

    // Producer threads
    dstd::Vector<dstd::Thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p, prod = channel.producer()]() mutable {
            auto guard = prod.guard();
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                int value = p * 1000 + i;
                CHECK(blocking_send(channel, value));
                total_produced.fetch_add(value);
            }
        });
    }

    // Consumer thread
    dstd::Thread consumer([&]() {
        int value;
        while (blocking_receive(channel, value)) {
            total_consumed.fetch_add(value);
        }
    });

    for (auto& t : producers) {
        t.join();
    }
    consumer.join();

    CHECK(total_produced.load() == total_consumed.load());
}

TEST_CASE("Channel - Single producer, multiple consumers" *
          doctest::test_suite("vg")) {
    Channel<int> channel(100);

    constexpr int NUM_CONSUMERS = 4;
    constexpr int NUM_ITEMS = 1000;
    std::atomic<int> sum_produced{0};
    std::atomic<int> sum_consumed{0};

    // Producer thread
    dstd::Thread producer([&, p = channel.producer()]() mutable {
        auto guard = p.guard();
        for (int i = 0; i < NUM_ITEMS; ++i) {
            CHECK(blocking_send(channel, i));
            sum_produced.fetch_add(i);
        }
    });

    // Consumer threads
    dstd::Vector<dstd::Thread> consumers;
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&]() {
            int value;
            while (blocking_receive(channel, value)) {
                sum_consumed.fetch_add(value);
            }
        });
    }

    producer.join();
    for (auto& t : consumers) {
        t.join();
    }

    CHECK(sum_produced.load() == sum_consumed.load());
}

// ============================================================================
// Channel with String Data
// ============================================================================

TEST_CASE("Channel - String messages") {
    Channel<dstd::String> channel(10);

    CHECK(channel.try_send("Hello"));
    CHECK(channel.try_send("World"));
    CHECK(channel.try_send("from"));
    CHECK(channel.try_send("Channel"));

    dstd::String msg;
    CHECK(blocking_receive(channel, msg) == true);
    CHECK(msg == "Hello");

    CHECK(blocking_receive(channel, msg) == true);
    CHECK(msg == "World");

    CHECK(blocking_receive(channel, msg) == true);
    CHECK(msg == "from");

    CHECK(blocking_receive(channel, msg) == true);
    CHECK(msg == "Channel");
}

TEST_CASE("Channel - Complex data type") {
    struct Event {
        int id;
        dstd::String name;
        double value;

        Event() : id(0), value(0.0) {}
        Event(int i, dstd::String n, double v)
            : id(i), name(std::move(n)), value(v) {}
    };

    Channel<Event> channel(10);

    CHECK(channel.try_send(Event{1, "start", 0.0}));
    CHECK(channel.try_send(Event{2, "process", 42.5}));
    CHECK(channel.try_send(Event{3, "end", 100.0}));

    Event evt;
    CHECK(blocking_receive(channel, evt) == true);
    CHECK(evt.id == 1);
    CHECK(evt.name == "start");

    CHECK(blocking_receive(channel, evt) == true);
    CHECK(evt.id == 2);
    CHECK(evt.value == 42.5);

    CHECK(blocking_receive(channel, evt) == true);
    CHECK(evt.id == 3);
    CHECK(evt.name == "end");
}

// ============================================================================
// Channel Between Tasks Tests
// ============================================================================

TEST_CASE("Channel - Tasks run in order") {
    Channel<int> channel(100);

    std::atomic<int> producer_sum{0};
    std::atomic<int> consumer_sum{0};

    auto producer_task =
        [&, p = channel.producer()]() mutable -> coro::CoroTask<void> {
        auto guard = p.guard();

        for (int i = 0; i < 100; ++i) {
            CHECK(co_await channel.send(i));
            producer_sum.fetch_add(i);
        }
        co_return;
    };

    auto consumer_task = [&]() -> coro::CoroTask<void> {
        int value;

        while (blocking_receive(channel, value)) {
            consumer_sum.fetch_add(value);
        }
        co_return;
    };

    auto in_order = [&]() -> coro::CoroTask<void> {
        co_await producer_task();
        co_await consumer_task();
    };

    Runtime rt(2);
    rt.block_on(in_order());

    CHECK(producer_sum.load() == consumer_sum.load());
}

TEST_CASE("Channel - Chained transform tasks") {
    Channel<int> input_channel(50);
    Channel<int> output_channel(50);

    dstd::Vector<int> produced_values;
    dstd::Vector<int> transformed_values;
    dstd::Vector<int> consumed_values;

    auto producer =
        [&, p = input_channel.producer()]() mutable -> coro::CoroTask<void> {
        auto guard = p.guard();

        for (int i = 1; i <= 10; ++i) {
            CHECK(co_await input_channel.send(i));
            produced_values.push_back(i);
        }
        co_return;
    };

    // Transform task (multiply by 2)
    auto transform =
        [&, op = output_channel.producer()]() mutable -> coro::CoroTask<void> {
        auto out_guard = op.guard();

        int value;
        while (blocking_receive(input_channel, value)) {
            int transformed = value * 2;
            CHECK(co_await output_channel.send(transformed));
            transformed_values.push_back(transformed);
        }
        co_return;
    };

    auto consumer = [&]() -> coro::CoroTask<void> {
        int value;
        while (blocking_receive(output_channel, value)) {
            consumed_values.push_back(value);
        }
        co_return;
    };

    auto in_order = [&]() -> coro::CoroTask<void> {
        co_await producer();
        co_await transform();
        co_await consumer();
    };

    Runtime rt(3);
    rt.block_on(in_order());

    CHECK(produced_values.size() == 10);
    CHECK(transformed_values.size() == 10);
    CHECK(consumed_values.size() == 10);

    // Verify transformation
    for (size_t i = 0; i < produced_values.size(); ++i) {
        CHECK(transformed_values[i] == produced_values[i] * 2);
        CHECK(consumed_values[i] == produced_values[i] * 2);
    }
}

TEST_CASE("Channel - Fan-out pattern (one producer, multiple consumers)") {
    Channel<int> channel(100);

    std::atomic<int> sum_produced{0};
    std::atomic<int> sum_consumer1{0};
    std::atomic<int> sum_consumer2{0};

    auto producer = [&,
                     p = channel.producer()]() mutable -> coro::CoroTask<void> {
        auto guard = p.guard();

        for (int i = 0; i < 100; ++i) {
            CHECK(co_await channel.send(i));
            sum_produced.fetch_add(i);
        }
        co_return;
    };

    auto consumer1 = [&]() -> coro::CoroTask<void> {
        int value;

        while (channel.try_receive(value)) {
            sum_consumer1.fetch_add(value);
        }
        co_return;
    };

    auto consumer2 = [&]() -> coro::CoroTask<void> {
        int value;

        while (blocking_receive(channel, value)) {
            sum_consumer2.fetch_add(value);
        }
        co_return;
    };

    // All tasks run in parallel: the producer fills the channel while the
    // consumers drain it concurrently.
    Runtime rt(3);
    dstd::Vector<coro::CoroTask<void>> tasks;
    tasks.push_back(producer());
    tasks.push_back(consumer1());
    tasks.push_back(consumer2());
    run_together(rt, std::move(tasks));

    // Both consumers should get all items (or split them)
    int total_consumed = sum_consumer1.load() + sum_consumer2.load();
    CHECK(total_consumed == sum_produced.load());
}

// ============================================================================
// Stress Tests
// ============================================================================

TEST_CASE("Channel - High throughput stress test") {
    Channel<int> channel(1000);

    constexpr int NUM_ITEMS = 10000;
    std::atomic<int> items_sent{0};
    std::atomic<int> items_received{0};

    dstd::Thread producer([&, p = channel.producer()]() mutable {
        auto guard = p.guard();
        for (int i = 0; i < NUM_ITEMS; ++i) {
            CHECK(blocking_send(channel, i));
            items_sent.fetch_add(1);
        }
    });

    dstd::Thread consumer([&]() {
        int value;
        while (blocking_receive(channel, value)) {
            items_received.fetch_add(1);
        }
    });

    producer.join();
    consumer.join();

    CHECK(items_sent.load() == NUM_ITEMS);
    CHECK(items_received.load() == NUM_ITEMS);
}

TEST_CASE("Channel - Rapid open/close cycles") {
    for (int cycle = 0; cycle < 10; ++cycle) {
        Channel<int> channel(10);

        auto p = channel.producer();
        auto guard = p.guard();

        for (int i = 0; i < 10; ++i) {
            CHECK(blocking_send(channel, i));
        }

        int value;
        int count = 0;
        while (channel.try_receive(value)) {
            count++;
        }

        CHECK(count == 10);
    }
}

TEST_CASE("Channel - close wakes blocked receive" * doctest::test_suite("vg")) {
    Channel<int> channel(10);

    std::atomic<bool> receiver_entered{false};
    std::atomic<bool> receive_done{false};
    bool receive_result = true;

    dstd::Thread receiver([&]() {
        int value = 0;
        receiver_entered.store(true, std::memory_order_release);
        receive_result = blocking_receive(channel, value);
        receive_done.store(true, std::memory_order_release);
    });

    while (!receiver_entered.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto t0 = std::chrono::steady_clock::now();
    channel.close();
    receiver.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    CHECK(receive_result == false);
    CHECK(receive_done.load(std::memory_order_acquire) == true);
    CHECK(channel.is_closed() == true);
    CHECK(elapsed.count() < 200);
}

TEST_CASE("Channel - last producer release wakes blocked receive") {
    Channel<int> channel(10);

    auto cp = channel.producer();
    auto guard = dstd::make_unique<Channel<int>::ProducerGuard>(cp.guard());

    std::atomic<bool> receiver_entered{false};
    bool receive_result = true;

    dstd::Thread receiver([&]() {
        int value = 0;
        receiver_entered.store(true, std::memory_order_release);
        receive_result = blocking_receive(channel, value);
    });

    while (!receiver_entered.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    CHECK(channel.producer_count() == 1);

    auto t0 = std::chrono::steady_clock::now();
    guard.reset();
    receiver.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    CHECK(receive_result == false);
    CHECK(channel.producer_count() == 0);
    CHECK(channel.is_closed() == true);
    CHECK(elapsed.count() < 200);
}

TEST_CASE("Channel - send_async unblocks when receiver drains") {
    auto channel = coro::make_channel<int>(2);

    std::atomic<bool> third_send_started{false};
    std::atomic<bool> third_send_completed{false};
    std::atomic<int> consumed_sum{0};

    auto producer =
        [&, p = channel->producer()]() mutable -> coro::CoroTask<void> {
        auto guard = p.guard();
        CHECK(co_await channel->send(1));
        CHECK(co_await channel->send(2));
        third_send_started.store(true, std::memory_order_release);
        CHECK(co_await channel->send(3));
        third_send_completed.store(true, std::memory_order_release);
        co_return;
    };

    auto consumer = [&]() -> coro::CoroTask<void> {
        while (!third_send_started.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            co_await coro::yield();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        co_await coro::yield();

        CHECK(third_send_completed.load(std::memory_order_acquire) == false);

        while (auto item = co_await channel->receive()) {
            consumed_sum.fetch_add(*item);
        }
        co_return;
    };

    Runtime rt(2);
    dstd::Vector<coro::CoroTask<void>> tasks;
    tasks.push_back(producer());
    tasks.push_back(consumer());
    run_together(rt, std::move(tasks));

    CHECK(third_send_started.load(std::memory_order_acquire) == true);
    CHECK(third_send_completed.load(std::memory_order_acquire) == true);
    CHECK(consumed_sum.load() == 6);
}

TEST_CASE("Channel - send_async resumes false when closed") {
    auto channel = coro::make_channel<int>(1);

    std::atomic<bool> second_send_started{false};
    std::atomic<bool> second_send_completed{false};
    std::atomic<bool> second_send_result{true};

    auto producer =
        [&, p = channel->producer()]() mutable -> coro::CoroTask<void> {
        auto guard = p.guard();
        CHECK(co_await channel->send(1));
        second_send_started.store(true, std::memory_order_release);
        const bool sent = co_await channel->send(2);
        second_send_result.store(sent, std::memory_order_release);
        second_send_completed.store(true, std::memory_order_release);
        co_return;
    };

    auto closer = [&]() -> coro::CoroTask<void> {
        while (!second_send_started.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            co_await coro::yield();
        }
        channel->close();
        co_return;
    };

    Runtime rt(2);
    dstd::Vector<coro::CoroTask<void>> tasks;
    tasks.push_back(producer());
    tasks.push_back(closer());
    run_together(rt, std::move(tasks));

    CHECK(second_send_started.load(std::memory_order_acquire) == true);
    CHECK(second_send_completed.load(std::memory_order_acquire) == true);
    CHECK(second_send_result.load(std::memory_order_acquire) == false);
}

TEST_CASE("Channel - async bounded handoff on single compute thread") {
    Channel<int> channel(1);

    const int item_count = TSAN_BUILD ? 1000 : 5000;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    auto producer = [&,
                     p = channel.producer()]() mutable -> coro::CoroTask<void> {
        auto guard = p.guard();
        for (int i = 1; i <= item_count; ++i) {
            CHECK(co_await channel.send(i));
            produced.fetch_add(i, std::memory_order_relaxed);
        }
        co_return;
    };

    auto consumer = [&]() -> coro::CoroTask<void> {
        while (auto item = co_await channel.receive()) {
            consumed.fetch_add(*item, std::memory_order_relaxed);
        }
        co_return;
    };

    Runtime rt(1);
    dstd::Vector<coro::CoroTask<void>> tasks;
    tasks.push_back(producer());
    tasks.push_back(consumer());
    run_together(rt, std::move(tasks));

    CHECK(produced.load(std::memory_order_relaxed) ==
          consumed.load(std::memory_order_relaxed));
    CHECK(consumed.load(std::memory_order_relaxed) ==
          (item_count * (item_count + 1)) / 2);
}

// ============================================================================
// Async I/O with receive_async() Tests
// ============================================================================

TEST_CASE("Channel - receive_async()") {
    Channel<int> channel(100);

    std::atomic<int> producer_sum{0};
    std::atomic<int> consumer_sum{0};

    auto producer = [&,
                     p = channel.producer()]() mutable -> coro::CoroTask<void> {
        auto guard = p.guard();

        for (int i = 0; i < 50; ++i) {
            CHECK(co_await channel.send(i));
            producer_sum.fetch_add(i);
        }
        co_return;
    };

    auto consumer = [&]() -> coro::CoroTask<void> {
        while (auto item_opt = co_await channel.receive()) {
            consumer_sum.fetch_add(*item_opt);
        }
        co_return;
    };

    Runtime rt(2);
    dstd::Vector<coro::CoroTask<void>> tasks;
    tasks.push_back(producer());
    tasks.push_back(consumer());
    run_together(rt, std::move(tasks));

    CHECK(producer_sum.load() == consumer_sum.load());
    CHECK(consumer_sum.load() == (49 * 50 / 2));  // sum of 0..49
}

TEST_CASE("Channel - receive_async() with multiple consumers") {
    Channel<int> channel(100);

    std::atomic<int> producer_sum{0};
    std::atomic<int> consumer1_sum{0};
    std::atomic<int> consumer2_sum{0};

    auto producer = [&,
                     p = channel.producer()]() mutable -> coro::CoroTask<void> {
        auto guard = p.guard();

        for (int i = 0; i < 100; ++i) {
            CHECK(co_await channel.send(i));
            producer_sum.fetch_add(i);
        }
        co_return;
    };

    auto consumer1 = [&]() -> coro::CoroTask<void> {
        while (auto item_opt = co_await channel.receive()) {
            consumer1_sum.fetch_add(*item_opt);
        }
        co_return;
    };

    auto consumer2 = [&]() -> coro::CoroTask<void> {
        while (auto item_opt = co_await channel.receive()) {
            consumer2_sum.fetch_add(*item_opt);
        }
        co_return;
    };

    Runtime rt(3);
    dstd::Vector<coro::CoroTask<void>> tasks;
    tasks.push_back(producer());
    tasks.push_back(consumer1());
    tasks.push_back(consumer2());
    run_together(rt, std::move(tasks));

    // Both consumers should split the items
    int total_consumed = consumer1_sum.load() + consumer2_sum.load();
    CHECK(total_consumed == producer_sum.load());
}

TEST_CASE("Channel - receive_async() with transform chain") {
    Channel<int> input_channel(50);
    Channel<int> output_channel(50);

    dstd::Vector<int> produced_values;
    dstd::Vector<int> transformed_values;
    dstd::Vector<int> consumed_values;

    auto producer =
        [&, p = input_channel.producer()]() mutable -> coro::CoroTask<void> {
        auto guard = p.guard();

        for (int i = 1; i <= 20; ++i) {
            CHECK(co_await input_channel.send(i));
            produced_values.push_back(i);
        }
        co_return;
    };

    // Transform using receive_async() (square the values)
    auto transform =
        [&, op = output_channel.producer()]() mutable -> coro::CoroTask<void> {
        auto out_guard = op.guard();

        while (auto item_opt = co_await input_channel.receive()) {
            int transformed = (*item_opt) * (*item_opt);  // Square
            CHECK(co_await output_channel.send(transformed));
            transformed_values.push_back(transformed);
        }
        co_return;
    };

    auto consumer = [&]() -> coro::CoroTask<void> {
        while (auto item_opt = co_await output_channel.receive()) {
            consumed_values.push_back(*item_opt);
        }
        co_return;
    };

    auto in_order = [&]() -> coro::CoroTask<void> {
        co_await producer();
        co_await transform();
        co_await consumer();
    };

    Runtime rt(3);
    rt.block_on(in_order());

    CHECK(produced_values.size() == 20);
    CHECK(transformed_values.size() == 20);
    CHECK(consumed_values.size() == 20);

    // Verify transformation (squares)
    for (size_t i = 0; i < produced_values.size(); ++i) {
        CHECK(transformed_values[i] == produced_values[i] * produced_values[i]);
        CHECK(consumed_values[i] == produced_values[i] * produced_values[i]);
    }
}

TEST_CASE("Channel - receive_async() fallback") {
    Channel<int> channel(50);

    std::atomic<int> producer_sum{0};
    std::atomic<int> consumer_sum{0};

    auto producer = [&,
                     p = channel.producer()]() mutable -> coro::CoroTask<void> {
        auto guard = p.guard();

        for (int i = 0; i < 30; ++i) {
            CHECK(co_await channel.send(i));
            producer_sum.fetch_add(i);
        }
        co_return;
    };

    auto consumer = [&]() -> coro::CoroTask<void> {
        while (auto item_opt = co_await channel.receive()) {
            consumer_sum.fetch_add(*item_opt);
        }
        co_return;
    };

    Runtime rt(2);
    dstd::Vector<coro::CoroTask<void>> tasks;
    tasks.push_back(producer());
    tasks.push_back(consumer());
    run_together(rt, std::move(tasks));

    CHECK(producer_sum.load() == consumer_sum.load());
    CHECK(consumer_sum.load() == (29 * 30 / 2));  // sum of 0..29
}

TEST_CASE("Channel - receive_async() stress test") {
    Channel<int> channel(200);

    std::atomic<int> items_sent{0};
    std::atomic<int> items_received{0};

    constexpr int NUM_ITEMS = 1000;

    auto producer = [&,
                     p = channel.producer()]() mutable -> coro::CoroTask<void> {
        auto guard = p.guard();

        for (int i = 0; i < NUM_ITEMS; ++i) {
            CHECK(co_await channel.send(i));
            items_sent.fetch_add(1);
        }
        co_return;
    };

    auto consumer = [&]() -> coro::CoroTask<void> {
        while (auto item_opt = co_await channel.receive()) {
            items_received.fetch_add(1);
        }
        co_return;
    };

    Runtime rt(2);
    dstd::Vector<coro::CoroTask<void>> tasks;
    tasks.push_back(producer());
    tasks.push_back(consumer());
    run_together(rt, std::move(tasks));

    CHECK(items_sent.load() == NUM_ITEMS);
    CHECK(items_received.load() == NUM_ITEMS);
}

TEST_CASE("Channel - Multiple producers, multiple consumers (blocking)") {
    Channel<int> channel(200);

    constexpr int NUM_PRODUCERS = 3;
    constexpr int NUM_CONSUMERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 100;

    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};

    auto make_producer = [&](int p, auto prod) -> coro::CoroTask<void> {
        auto guard = prod.guard();

        for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
            int value = p * 1000 + i;
            CHECK(co_await channel.send(value));
            total_produced.fetch_add(value);
        }
    };

    auto consume = [&]() -> coro::CoroTask<void> {
        int value;

        while (blocking_receive(channel, value)) {
            total_consumed.fetch_add(value);
        }
        co_return;
    };

    Runtime rt(NUM_PRODUCERS + NUM_CONSUMERS);
    dstd::Vector<coro::CoroTask<void>> tasks;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        tasks.push_back(make_producer(p, channel.producer()));
    }
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        tasks.push_back(consume());
    }
    run_together(rt, std::move(tasks));

    // Verify all items were produced and consumed
    CHECK(total_produced.load() == total_consumed.load());
    int expected_sum = 0;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
            expected_sum += p * 1000 + i;
        }
    }
    CHECK(total_produced.load() == expected_sum);
}

TEST_CASE(
    "Channel - Multiple producers, multiple consumers with receive_async()") {
    Channel<int> channel(200);

    constexpr int NUM_PRODUCERS = 3;
    constexpr int NUM_CONSUMERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 100;

    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};

    auto make_producer = [&](int p, auto prod) -> coro::CoroTask<void> {
        auto guard = prod.guard();

        for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
            int value = p * 1000 + i;
            CHECK(co_await channel.send(value));
            total_produced.fetch_add(value);
        }
    };

    auto consume = [&]() -> coro::CoroTask<void> {
        while (auto item_opt = co_await channel.receive()) {
            total_consumed.fetch_add(*item_opt);
        }
        co_return;
    };

    Runtime rt(NUM_PRODUCERS + NUM_CONSUMERS);
    dstd::Vector<coro::CoroTask<void>> tasks;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        tasks.push_back(make_producer(p, channel.producer()));
    }
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        tasks.push_back(consume());
    }
    run_together(rt, std::move(tasks));

    // Verify all items were produced and consumed
    CHECK(total_produced.load() == total_consumed.load());
    int expected_sum = 0;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
            expected_sum += p * 1000 + i;
        }
    }
    CHECK(total_produced.load() == expected_sum);
}

// ============================================================================
// producer() and ProducerGuard Tests
// ============================================================================

TEST_CASE("Channel - producer() basic") {
    Channel<int> channel(10);

    CHECK(channel.producer_count() == 0);

    {
        auto p1 = channel.producer();
        auto guard1 = p1.guard();
        CHECK(channel.producer_count() == 1);

        {
            auto p2 = channel.producer();
            auto guard2 = p2.guard();
            CHECK(channel.producer_count() == 2);
        }

        CHECK(channel.producer_count() == 1);
    }
    // guard destroyed -> released
    CHECK(channel.producer_count() == 0);
    CHECK(channel.is_closed() == true);
}

TEST_CASE("Channel - producer() bulk") {
    Channel<int> channel(10);

    CHECK(channel.producer_count() == 0);

    // Create and release multiple producers
    {
        auto p1 = channel.producer();
        auto guard1 = p1.guard();
        CHECK(channel.producer_count() == 1);

        auto p2 = channel.producer();
        auto guard2 = p2.guard();
        CHECK(channel.producer_count() == 2);

        auto p3 = channel.producer();
        auto guard3 = p3.guard();
        CHECK(channel.producer_count() == 3);

        auto p4 = channel.producer();
        auto guard4 = p4.guard();
        CHECK(channel.producer_count() == 4);

        auto p5 = channel.producer();
        auto guard5 = p5.guard();
        CHECK(channel.producer_count() == 5);
    }
    // All released
    CHECK(channel.producer_count() == 0);
    CHECK(channel.is_closed() == true);
}

TEST_CASE("Channel - producer() guard extends shared_ptr lifetime") {
    // Verify that ProducerGuard keeps the channel alive even after
    // the original shared_ptr is released.
    auto channel = coro::make_channel<int>(10);

    // Create a producer and get a guard
    auto prod = channel->producer();
    auto guard = prod.guard();

    // Send a value while channel is alive
    CHECK(blocking_send(*channel, 42));

    // Release the original shared_ptr -- channel must stay alive
    // because the guard holds a shared_ptr internally.
    channel.reset();

    // Guard destructor runs here, accesses channel's mutex to
    // call release_producer() + notify_all_waiters().
    // If the guard didn't hold a shared_ptr, this would crash
    // with "mutex lock failed: Invalid argument" on macOS.
}

TEST_CASE("Channel - producer() with threads") {
    Channel<int> channel(100);

    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 250;
    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};

    // Producer threads use producer()
    dstd::Vector<dstd::Thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p, prod = channel.producer()]() mutable {
            auto guard = prod.guard();  // RAII release
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                int value = p * 1000 + i;
                CHECK(blocking_send(channel, value));
                total_produced.fetch_add(value);
            }
        });
    }

    // Consumer thread
    dstd::Thread consumer([&]() {
        int value;
        while (blocking_receive(channel, value)) {
            total_consumed.fetch_add(value);
        }
    });

    for (auto& t : producers) t.join();
    consumer.join();

    CHECK(total_produced.load() == total_consumed.load());
}

TEST_CASE("Channel - producer() with scope.spawn() pattern") {
    auto channel = coro::make_channel<int>(100);

    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 50;
    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};

    Runtime rt(NUM_PRODUCERS + 1);
    rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            // Spawn producers using producer()
            for (int p = 0; p < NUM_PRODUCERS; ++p) {
                scope.spawn([&, p, prod = channel->producer()]() mutable
                                -> coro::CoroTask<void> {
                    auto guard = prod.guard();
                    for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                        int value = p * 1000 + i;
                        CHECK(co_await channel->send(value));
                        total_produced.fetch_add(value);
                    }
                    co_return;
                });
            }

            // Consumer coroutine
            auto* total_consumed_ptr = &total_consumed;
            scope.spawn(
                [total_consumed_ptr, channel]() -> coro::CoroTask<void> {
                    while (auto item = co_await channel->receive()) {
                        total_consumed_ptr->fetch_add(*item);
                    }
                    co_return;
                });

            co_return;
        }));

    CHECK(total_produced.load() == total_consumed.load());
    int expected_sum = 0;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
            expected_sum += p * 1000 + i;
        }
    }
    CHECK(total_produced.load() == expected_sum);
}

TEST_CASE("Channel - producer() early exit in scope.spawn()") {
    auto channel = coro::make_channel<int>(100);

    constexpr int NUM_PRODUCERS = 4;
    std::atomic<int> total_consumed{0};

    Runtime rt(NUM_PRODUCERS + 1);
    rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            // Some producers exit early (simulating skip/error)
            for (int p = 0; p < NUM_PRODUCERS; ++p) {
                scope.spawn([&, p, prod = channel->producer()]() mutable
                                -> coro::CoroTask<void> {
                    auto guard = prod.guard();
                    if (p % 2 == 0) {
                        // Early exit - guard still releases
                        co_return;
                    }
                    for (int i = 0; i < 10; ++i) {
                        CHECK(co_await channel->send(p * 100 + i));
                    }
                    co_return;
                });
            }

            // Consumer
            auto* total_consumed_ptr = &total_consumed;
            scope.spawn(
                [total_consumed_ptr, channel]() -> coro::CoroTask<void> {
                    while (auto item = co_await channel->receive()) {
                        total_consumed_ptr->fetch_add(1);
                    }
                    co_return;
                });

            co_return;
        }));

    // Only producers 1 and 3 send 10 items each
    CHECK(total_consumed.load() == 20);
}

TEST_CASE("Channel - producer() two-stage pipeline with scope.spawn()") {
    auto stage1 = coro::make_channel<int>(50);
    auto stage2 = coro::make_channel<int>(50);

    constexpr int NUM_PRODUCERS = 3;
    constexpr int NUM_WORKERS = 2;
    constexpr int ITEMS_PER_PRODUCER = 20;
    std::atomic<int> total_consumed{0};

    Runtime rt(NUM_PRODUCERS + NUM_WORKERS + 1);
    rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            // Stage 1: producers -> stage1 channel
            for (int p = 0; p < NUM_PRODUCERS; ++p) {
                scope.spawn([&, p, prod = stage1->producer()]() mutable
                                -> coro::CoroTask<void> {
                    auto guard = prod.guard();
                    for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                        CHECK(co_await stage1->send(p * 1000 + i));
                    }
                    co_return;
                });
            }

            // Stage 2: workers read stage1, write stage2
            auto* stage1_ptr = &stage1;
            auto* stage2_ptr = &stage2;
            for (int w = 0; w < NUM_WORKERS; ++w) {
                scope.spawn([stage1_ptr, prod = stage2->producer()]() mutable
                                -> coro::CoroTask<void> {
                    auto guard = prod.guard();
                    while (auto item = co_await (*stage1_ptr)->receive()) {
                        CHECK(co_await prod.send(*item * 2));
                    }
                    co_return;
                });
            }

            // Final consumer
            auto* total_consumed_ptr = &total_consumed;
            scope.spawn(
                [total_consumed_ptr, stage2_ptr]() -> coro::CoroTask<void> {
                    while (auto item = co_await (*stage2_ptr)->receive()) {
                        total_consumed_ptr->fetch_add(1);
                    }
                    co_return;
                });

            co_return;
        }));

    CHECK(total_consumed.load() == NUM_PRODUCERS * ITEMS_PER_PRODUCER);
}

// ============================================================================
// spawn_transforms() Tests
// ============================================================================

TEST_CASE("Channel - spawn_transforms() basic") {
    auto input = coro::make_channel<int>(50);
    auto output = coro::make_channel<int>(50);

    constexpr int NUM_ITEMS = 100;
    std::atomic<int> total_consumed{0};

    Runtime rt(4);
    rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            // Single producer
            auto generate = []() -> coro::Generator<int> {
                for (int i = 1; i <= NUM_ITEMS; ++i) {
                    co_yield i;
                }
            };
            scope.spawn([&]() -> coro::CoroTask<void> {
                auto ch = input->producer();
                auto guard = ch.guard();
                coro::Generator<int> gen = generate();
                for (auto item : gen) {
                    const auto send_ok = co_await ch.send(std::move(item));
                    if (!send_ok) {
                        co_return;
                    }
                }
                co_return;
            });

            // Transform: square each value
            auto square = [](int val) -> coro::CoroTask<int> {
                co_return val* val;
            };
            for (int w = 0; w < 4; ++w) {
                scope.spawn([&]() -> coro::CoroTask<void> {
                    auto op = output->producer();
                    auto guard = op.guard();
                    for (;;) {
                        auto item = co_await input->receive();
                        if (!item) break;
                        auto result = co_await square(std::move(*item));
                        const auto send_ok =
                            co_await op.send(std::move(result));
                        if (!send_ok) {
                            co_return;
                        }
                    }
                    co_return;
                });
            }

            // Consumer
            scope.spawn([&]() -> coro::CoroTask<void> {
                for (;;) {
                    auto item = co_await output->receive();
                    if (!item) break;
                    total_consumed.fetch_add(*item);
                }
                co_return;
            });

            co_return;
        }));

    // sum of squares 1..100
    int expected = 0;
    for (int i = 1; i <= NUM_ITEMS; ++i) expected += i * i;
    CHECK(total_consumed.load() == expected);
}

TEST_CASE("Channel - spawn_transforms() two-stage pipeline") {
    auto stage1 = coro::make_channel<int>(50);
    auto stage2 = coro::make_channel<dstd::String>(50);
    auto stage3 = coro::make_channel<dstd::String>(50);

    std::atomic<int> total_consumed{0};

    Runtime rt(8);
    rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            // Producer: emit integers
            auto generate = []() -> coro::Generator<int> {
                for (int i = 0; i < 50; ++i) co_yield i;
            };
            scope.spawn([&]() -> coro::CoroTask<void> {
                auto ch = stage1->producer();
                auto guard = ch.guard();
                coro::Generator<int> gen = generate();
                for (auto item : gen) {
                    const auto send_ok = co_await ch.send(std::move(item));
                    if (!send_ok) {
                        co_return;
                    }
                }
                co_return;
            });

            // Transform 1: int -> string
            auto to_string_op = [](int val) -> coro::CoroTask<dstd::String> {
                co_return dstd::to_string(val);
            };
            for (int w = 0; w < 2; ++w) {
                scope.spawn([&]() -> coro::CoroTask<void> {
                    auto op = stage2->producer();
                    auto guard = op.guard();
                    for (;;) {
                        auto item = co_await stage1->receive();
                        if (!item) break;
                        auto result = co_await to_string_op(std::move(*item));
                        const auto send_ok =
                            co_await op.send(std::move(result));
                        if (!send_ok) {
                            co_return;
                        }
                    }
                    co_return;
                });
            }

            // Transform 2: string -> string (prefix)
            auto prefix = [](dstd::String val) -> coro::CoroTask<dstd::String> {
                co_return "item_" + val;
            };
            for (int w = 0; w < 2; ++w) {
                scope.spawn([&]() -> coro::CoroTask<void> {
                    auto op = stage3->producer();
                    auto guard = op.guard();
                    for (;;) {
                        auto item = co_await stage2->receive();
                        if (!item) break;
                        auto result = co_await prefix(std::move(*item));
                        const auto send_ok =
                            co_await op.send(std::move(result));
                        if (!send_ok) {
                            co_return;
                        }
                    }
                    co_return;
                });
            }

            // Consumer
            scope.spawn([&]() -> coro::CoroTask<void> {
                for (;;) {
                    auto item = co_await stage3->receive();
                    if (!item) break;
                    total_consumed.fetch_add(1);
                }
                co_return;
            });

            co_return;
        }));

    CHECK(total_consumed.load() == 50);
}
