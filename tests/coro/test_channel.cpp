#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/yield.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;

namespace {

#if defined(__SANITIZE_THREAD__)
constexpr bool kTsanBuild = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool kTsanBuild = true;
#else
constexpr bool kTsanBuild = false;
#endif
#else
constexpr bool kTsanBuild = false;
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

TEST_CASE("Channel - Send and receive") {
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

    CHECK(channel.num_producers() == 0);

    {
        auto guard1 = channel.producer_guard();
        CHECK(channel.num_producers() == 1);

        {
            auto guard2 = channel.producer_guard();
            CHECK(channel.num_producers() == 2);
        }

        CHECK(channel.num_producers() == 1);
    }

    CHECK(channel.num_producers() == 0);
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
    std::thread producer([&]() {
        auto guard = channel.producer_guard();
        for (int i = 0; i < NUM_ITEMS; ++i) {
            if (blocking_send(channel, i)) sum_produced.fetch_add(i);
        }
    });

    // Consumer thread
    std::thread consumer([&]() {
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

    std::thread consumer([&]() {
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

    std::thread producer([&]() {
        auto guard = channel.producer_guard();
        CHECK(channel.num_producers() >= 1);
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

TEST_CASE("Channel - Multiple producers, single consumer") {
    Channel<int> channel(100);

    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 250;
    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};

    channel.register_producers(NUM_PRODUCERS);

    // Producer threads
    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            auto guard = channel.adopt_producer();
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                int value = p * 1000 + i;
                CHECK(blocking_send(channel, value));
                total_produced.fetch_add(value);
            }
        });
    }

    // Consumer thread
    std::thread consumer([&]() {
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

TEST_CASE("Channel - Single producer, multiple consumers") {
    Channel<int> channel(100);

    constexpr int NUM_CONSUMERS = 4;
    constexpr int NUM_ITEMS = 1000;
    std::atomic<int> sum_produced{0};
    std::atomic<int> sum_consumed{0};

    // Producer thread
    std::thread producer([&]() {
        auto guard = channel.producer_guard();
        for (int i = 0; i < NUM_ITEMS; ++i) {
            CHECK(blocking_send(channel, i));
            sum_produced.fetch_add(i);
        }
    });

    // Consumer threads
    std::vector<std::thread> consumers;
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
    Channel<std::string> channel(10);

    CHECK(channel.try_send("Hello"));
    CHECK(channel.try_send("World"));
    CHECK(channel.try_send("from"));
    CHECK(channel.try_send("Channel"));

    std::string msg;
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
        std::string name;
        double value;

        Event() : id(0), value(0.0) {}
        Event(int i, std::string n, double v)
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
// Channel with Pipeline Integration Tests
// ============================================================================

TEST_CASE("Channel - With tasks in pipeline") {
    Channel<int> channel(100);

    std::atomic<int> producer_sum{0};
    std::atomic<int> consumer_sum{0};

    // Create producer task
    auto producer_task = make_task(
        [&]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
            auto guard = channel.producer_guard();

            for (int i = 0; i < 100; ++i) {
                CHECK(co_await channel.send(i));
                producer_sum.fetch_add(i);
            }

            co_return 100;
        },
        "Producer");

    // Create consumer task
    auto consumer_task = make_task(
        [&]([[maybe_unused]] CoroScope& ctx,
            [[maybe_unused]] int count) -> coro::CoroTask<int> {
            int value;
            int items_consumed = 0;

            while (blocking_receive(channel, value)) {
                consumer_sum.fetch_add(value);
                items_consumed++;
            }

            co_return items_consumed;
        },
        "Consumer");

    consumer_task->depends_on(producer_task);

    // Execute in pipeline
    auto config =
        PipelineConfig().with_name("ChannelTest").with_compute_threads(2);

    Pipeline pipeline(config);
    pipeline.set_source(producer_task);
    pipeline.set_destination(consumer_task);

    auto output = pipeline.execute();

    // Success if no exception thrown
    CHECK(producer_sum.load() == consumer_sum.load());
}

TEST_CASE("Channel - Pipeline with transform") {
    Channel<int> input_channel(50);
    Channel<int> output_channel(50);

    std::vector<int> produced_values;
    std::vector<int> transformed_values;
    std::vector<int> consumed_values;

    // Producer task
    auto producer = make_task(
        [&]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
            auto guard = input_channel.producer_guard();

            for (int i = 1; i <= 10; ++i) {
                CHECK(co_await input_channel.send(i));
                produced_values.push_back(i);
            }

            co_return 10;
        },
        "Producer");

    // Transform task (multiply by 2)
    auto transform = make_task(
        [&]([[maybe_unused]] CoroScope& ctx,
            [[maybe_unused]] int count) -> coro::CoroTask<int> {
            auto out_guard = output_channel.producer_guard();

            int value;
            while (blocking_receive(input_channel, value)) {
                int transformed = value * 2;
                CHECK(co_await output_channel.send(transformed));
                transformed_values.push_back(transformed);
            }

            co_return static_cast<int>(transformed_values.size());
        },
        "Transform");

    // Consumer task
    auto consumer = make_task(
        [&]([[maybe_unused]] CoroScope& ctx,
            [[maybe_unused]] int count) -> coro::CoroTask<int> {
            int value;
            while (blocking_receive(output_channel, value)) {
                consumed_values.push_back(value);
            }

            co_return static_cast<int>(consumed_values.size());
        },
        "Consumer");

    transform->depends_on(producer);
    consumer->depends_on(transform);

    // Execute pipeline
    auto config =
        PipelineConfig().with_name("TransformPipeline").with_compute_threads(3);

    Pipeline pipeline(config);
    pipeline.set_source(producer);
    pipeline.set_destination(consumer);

    auto output = pipeline.execute();

    // Success if no exception thrown
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

    // Producer
    auto producer = make_task(
        [&]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
            auto guard = channel.producer_guard();

            for (int i = 0; i < 100; ++i) {
                CHECK(co_await channel.send(i));
                sum_produced.fetch_add(i);
            }

            co_return 100;
        },
        "Producer");

    // Consumer 1 - no input parameter since it's a source task
    auto consumer1 = make_task(
        [&]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
            int value;
            int items = 0;

            while (channel.try_receive(value)) {
                sum_consumer1.fetch_add(value);
                items++;
            }

            co_return items;
        },
        "Consumer1");

    // Consumer 2 - no input parameter since it's a source task
    auto consumer2 = make_task(
        [&]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
            int value;
            int items = 0;

            while (blocking_receive(channel, value)) {
                sum_consumer2.fetch_add(value);
                items++;
            }

            co_return items;
        },
        "Consumer2");

    // No dependencies - all tasks run in parallel
    // Producer fills the channel, consumers drain it concurrently

    // Execute pipeline
    auto config =
        PipelineConfig().with_name("FanOutPipeline").with_compute_threads(3);

    Pipeline pipeline(config);
    pipeline.set_source({producer, consumer1, consumer2});

    auto output = pipeline.execute();

    // Success if no exception thrown
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

    std::thread producer([&]() {
        auto guard = channel.producer_guard();
        for (int i = 0; i < NUM_ITEMS; ++i) {
            CHECK(blocking_send(channel, i));
            items_sent.fetch_add(1);
        }
    });

    std::thread consumer([&]() {
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

        auto guard = channel.producer_guard();

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

TEST_CASE("Channel - close wakes blocked receive") {
    Channel<int> channel(10);

    std::atomic<bool> receiver_entered{false};
    std::atomic<bool> receive_done{false};
    bool receive_result = true;

    std::thread receiver([&]() {
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
    CHECK(elapsed.count() < 50);
}

TEST_CASE("Channel - last producer release wakes blocked receive") {
    Channel<int> channel(10);

    auto guard =
        std::make_unique<Channel<int>::ProducerGuard>(channel.producer_guard());

    std::atomic<bool> receiver_entered{false};
    bool receive_result = true;

    std::thread receiver([&]() {
        int value = 0;
        receiver_entered.store(true, std::memory_order_release);
        receive_result = blocking_receive(channel, value);
    });

    while (!receiver_entered.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    CHECK(channel.num_producers() == 1);

    auto t0 = std::chrono::steady_clock::now();
    guard.reset();
    receiver.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    CHECK(receive_result == false);
    CHECK(channel.num_producers() == 0);
    CHECK(channel.is_closed() == true);
    CHECK(elapsed.count() < 50);
}

TEST_CASE("Channel - send_async unblocks when receiver drains") {
    auto channel = coro::make_channel<int>(2);

    std::atomic<bool> third_send_started{false};
    std::atomic<bool> third_send_completed{false};
    std::atomic<int> consumed_sum{0};

    auto producer = make_task(
        [&](CoroScope& /*ctx*/) -> coro::CoroTask<void> {
            auto guard = channel->producer_guard();
            CHECK(co_await channel->send(1));
            CHECK(co_await channel->send(2));
            third_send_started.store(true, std::memory_order_release);
            CHECK(co_await channel->send(3));
            third_send_completed.store(true, std::memory_order_release);
            co_return;
        },
        "AsyncSendProducer");

    auto consumer = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            while (!third_send_started.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                co_await coro::yield();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            co_await coro::yield();

            CHECK(third_send_completed.load(std::memory_order_acquire) ==
                  false);

            while (auto item = co_await ctx.receive(channel)) {
                consumed_sum.fetch_add(*item);
            }
            co_return;
        },
        "AsyncSendConsumer");

    auto config =
        PipelineConfig().with_name("AsyncSendDrain").with_compute_threads(2);

    Pipeline pipeline(config);
    pipeline.set_source({producer, consumer});
    pipeline.execute();

    CHECK(third_send_started.load(std::memory_order_acquire) == true);
    CHECK(third_send_completed.load(std::memory_order_acquire) == true);
    CHECK(consumed_sum.load() == 6);
}

TEST_CASE("Channel - send_async resumes false when closed") {
    auto channel = coro::make_channel<int>(1);

    std::atomic<bool> second_send_started{false};
    std::atomic<bool> second_send_completed{false};
    std::atomic<bool> second_send_result{true};

    auto producer = make_task(
        [&](CoroScope& /*ctx*/) -> coro::CoroTask<void> {
            auto guard = channel->producer_guard();
            CHECK(co_await channel->send(1));
            second_send_started.store(true, std::memory_order_release);
            const bool sent = co_await channel->send(2);
            second_send_result.store(sent, std::memory_order_release);
            second_send_completed.store(true, std::memory_order_release);
            co_return;
        },
        "AsyncSendCloseProducer");

    auto closer = make_task(
        [&](CoroScope& /* ctx */) -> coro::CoroTask<void> {
            while (!second_send_started.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                co_await coro::yield();
            }
            channel->close();
            co_return;
        },
        "AsyncSendCloser");

    auto config =
        PipelineConfig().with_name("AsyncSendClose").with_compute_threads(2);

    Pipeline pipeline(config);
    pipeline.set_source({producer, closer});
    pipeline.execute();

    CHECK(second_send_started.load(std::memory_order_acquire) == true);
    CHECK(second_send_completed.load(std::memory_order_acquire) == true);
    CHECK(second_send_result.load(std::memory_order_acquire) == false);
}

TEST_CASE("Channel - async bounded handoff on single compute thread") {
    Channel<int> channel(1);

    const int num_items = kTsanBuild ? 1000 : 5000;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    auto producer = make_task(
        [&](CoroScope& /*ctx*/) -> coro::CoroTask<void> {
            auto guard = channel.producer_guard();
            for (int i = 1; i <= num_items; ++i) {
                CHECK(co_await channel.send(i));
                produced.fetch_add(i, std::memory_order_relaxed);
            }
            co_return;
        },
        "SingleThreadAsyncProducer");

    auto consumer = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            while (auto item = co_await ctx.receive(channel)) {
                consumed.fetch_add(*item, std::memory_order_relaxed);
            }
            co_return;
        },
        "SingleThreadAsyncConsumer");

    auto config = PipelineConfig()
                      .with_name("SingleThreadAsyncHandoff")
                      .with_compute_threads(1);

    Pipeline pipeline(config);
    pipeline.set_source({producer, consumer});
    pipeline.execute();

    CHECK(produced.load(std::memory_order_relaxed) ==
          consumed.load(std::memory_order_relaxed));
    CHECK(consumed.load(std::memory_order_relaxed) ==
          (num_items * (num_items + 1)) / 2);
}

// ============================================================================
// Async I/O with receive_async() Tests
// ============================================================================

TEST_CASE("Channel - receive_async()") {
    Channel<int> channel(100);

    std::atomic<int> producer_sum{0};
    std::atomic<int> consumer_sum{0};

    // Producer task
    auto producer = make_task(
        [&]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
            auto guard = channel.producer_guard();

            for (int i = 0; i < 50; ++i) {
                CHECK(co_await channel.send(i));
                producer_sum.fetch_add(i);
            }

            co_return 50;
        },
        "Producer");

    // Consumer task using receive_async()
    auto consumer = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<int> {
            int items_consumed = 0;

            while (auto item_opt = co_await ctx.receive(channel)) {
                consumer_sum.fetch_add(*item_opt);
                items_consumed++;
            }

            co_return items_consumed;
        },
        "Consumer");

    // No dependencies - producer and consumer run in parallel

    auto config =
        PipelineConfig().with_name("AsyncReceiveTest").with_compute_threads(2);

    Pipeline pipeline(config);
    pipeline.set_source({producer, consumer});

    auto output = pipeline.execute();

    CHECK(producer_sum.load() == consumer_sum.load());
    CHECK(consumer_sum.load() == (49 * 50 / 2));  // sum of 0..49
}

TEST_CASE("Channel - receive_async() with multiple consumers") {
    Channel<int> channel(100);

    std::atomic<int> producer_sum{0};
    std::atomic<int> consumer1_sum{0};
    std::atomic<int> consumer2_sum{0};

    // Producer task
    auto producer = make_task(
        [&]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
            auto guard = channel.producer_guard();

            for (int i = 0; i < 100; ++i) {
                CHECK(co_await channel.send(i));
                producer_sum.fetch_add(i);
            }

            co_return 100;
        },
        "Producer");

    // Consumer 1 using receive_async()
    auto consumer1 = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<int> {
            int items = 0;

            while (auto item_opt = co_await ctx.receive(channel)) {
                consumer1_sum.fetch_add(*item_opt);
                items++;
            }

            co_return items;
        },
        "Consumer1");

    // Consumer 2 using receive_async()
    auto consumer2 = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<int> {
            int items = 0;

            while (auto item_opt = co_await ctx.receive(channel)) {
                consumer2_sum.fetch_add(*item_opt);
                items++;
            }

            co_return items;
        },
        "Consumer2");

    auto config = PipelineConfig()
                      .with_name("MultiConsumerAsync")
                      .with_compute_threads(3);

    Pipeline pipeline(config);
    pipeline.set_source({producer, consumer1, consumer2});

    auto output = pipeline.execute();

    // Both consumers should split the items
    int total_consumed = consumer1_sum.load() + consumer2_sum.load();
    CHECK(total_consumed == producer_sum.load());
}

TEST_CASE("Channel - receive_async() with transform pipeline") {
    Channel<int> input_channel(50);
    Channel<int> output_channel(50);

    std::vector<int> produced_values;
    std::vector<int> transformed_values;
    std::vector<int> consumed_values;

    // Producer task
    auto producer = make_task(
        [&]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
            auto guard = input_channel.producer_guard();

            for (int i = 1; i <= 20; ++i) {
                CHECK(co_await input_channel.send(i));
                produced_values.push_back(i);
            }

            co_return 20;
        },
        "Producer");

    // Transform task using receive_async() (square the values)
    auto transform = make_task(
        [&](CoroScope& ctx, [[maybe_unused]] int count) -> coro::CoroTask<int> {
            auto out_guard = output_channel.producer_guard();

            while (auto item_opt = co_await ctx.receive(input_channel)) {
                int transformed = (*item_opt) * (*item_opt);  // Square
                CHECK(co_await output_channel.send(transformed));
                transformed_values.push_back(transformed);
            }

            co_return static_cast<int>(transformed_values.size());
        },
        "Transform");

    // Consumer task using receive_async()
    auto consumer = make_task(
        [&](CoroScope& ctx, [[maybe_unused]] int count) -> coro::CoroTask<int> {
            while (auto item_opt = co_await ctx.receive(output_channel)) {
                consumed_values.push_back(*item_opt);
            }

            co_return static_cast<int>(consumed_values.size());
        },
        "Consumer");

    transform->depends_on(producer);
    consumer->depends_on(transform);

    auto config = PipelineConfig()
                      .with_name("AsyncTransformPipeline")
                      .with_compute_threads(3);

    Pipeline pipeline(config);
    pipeline.set_source(producer);
    pipeline.set_destination(consumer);

    auto output = pipeline.execute();

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

    // Producer task
    auto producer = make_task(
        [&]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
            auto guard = channel.producer_guard();

            for (int i = 0; i < 30; ++i) {
                CHECK(co_await channel.send(i));
                producer_sum.fetch_add(i);
            }

            co_return 30;
        },
        "Producer");

    // Consumer task using receive_async()
    auto consumer = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<int> {
            int items_consumed = 0;

            while (auto item_opt = co_await ctx.receive(channel)) {
                consumer_sum.fetch_add(*item_opt);
                items_consumed++;
            }

            co_return items_consumed;
        },
        "Consumer");

    // No dependencies - producer and consumer run in parallel

    auto config = PipelineConfig()
                      .with_name("AsyncReceiveFallback")
                      .with_compute_threads(2);

    Pipeline pipeline(config);
    pipeline.set_source({producer, consumer});

    auto output = pipeline.execute();

    CHECK(producer_sum.load() == consumer_sum.load());
    CHECK(consumer_sum.load() == (29 * 30 / 2));  // sum of 0..29
}

TEST_CASE("Channel - receive_async() stress test") {
    Channel<int> channel(200);

    std::atomic<int> items_sent{0};
    std::atomic<int> items_received{0};

    constexpr int NUM_ITEMS = 1000;

    // Producer task
    auto producer = make_task(
        [&]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
            auto guard = channel.producer_guard();

            for (int i = 0; i < NUM_ITEMS; ++i) {
                CHECK(co_await channel.send(i));
                items_sent.fetch_add(1);
            }

            co_return NUM_ITEMS;
        },
        "Producer");

    // Consumer task using receive_async()
    auto consumer = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<int> {
            int items = 0;

            while (auto item_opt = co_await ctx.receive(channel)) {
                items_received.fetch_add(1);
                items++;
            }

            co_return items;
        },
        "Consumer");

    // No dependencies - producer and consumer run in parallel

    auto config = PipelineConfig()
                      .with_name("AsyncReceiveStress")
                      .with_compute_threads(2);

    Pipeline pipeline(config);
    pipeline.set_source({producer, consumer});

    auto output = pipeline.execute();

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

    std::vector<std::shared_ptr<Task>> producers;
    std::vector<std::shared_ptr<Task>> consumers;

    channel.register_producers(NUM_PRODUCERS);

    // Create multiple producers
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        auto producer = make_task(
            [&, p]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
                auto guard = channel.adopt_producer();

                for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                    int value = p * 1000 + i;
                    CHECK(co_await channel.send(value));
                    total_produced.fetch_add(value);
                }

                co_return ITEMS_PER_PRODUCER;
            },
            "Producer" + std::to_string(p));
        producers.push_back(producer);
    }

    // Create multiple consumers
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        auto consumer = make_task(
            [&]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
                int items = 0;
                int value;

                while (blocking_receive(channel, value)) {
                    total_consumed.fetch_add(value);
                    items++;
                }

                co_return items;
            },
            "Consumer" + std::to_string(c));
        consumers.push_back(consumer);
    }

    // Execute all tasks in parallel
    auto config = PipelineConfig()
                      .with_name("MultiProducerMultiConsumer")
                      .with_compute_threads(NUM_PRODUCERS + NUM_CONSUMERS);

    Pipeline pipeline(config);

    // Combine all producers and consumers as source tasks
    std::vector<std::shared_ptr<Task>> all_tasks;
    all_tasks.insert(all_tasks.end(), producers.begin(), producers.end());
    all_tasks.insert(all_tasks.end(), consumers.begin(), consumers.end());

    pipeline.set_source(all_tasks);

    auto output = pipeline.execute();

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

    std::vector<std::shared_ptr<Task>> producers;
    std::vector<std::shared_ptr<Task>> consumers;

    channel.register_producers(NUM_PRODUCERS);

    // Create multiple producers
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        auto producer = make_task(
            [&, p]([[maybe_unused]] CoroScope& ctx) -> coro::CoroTask<int> {
                auto guard = channel.adopt_producer();

                for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                    int value = p * 1000 + i;
                    CHECK(co_await channel.send(value));
                    total_produced.fetch_add(value);
                }

                co_return ITEMS_PER_PRODUCER;
            },
            "Producer" + std::to_string(p));
        producers.push_back(producer);
    }

    // Create multiple consumers using receive_async()
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        auto consumer = make_task(
            [&](CoroScope& ctx) -> coro::CoroTask<int> {
                int items = 0;

                while (auto item_opt = co_await ctx.receive(channel)) {
                    total_consumed.fetch_add(*item_opt);
                    items++;
                }

                co_return items;
            },
            "Consumer" + std::to_string(c));
        consumers.push_back(consumer);
    }

    // Execute all tasks in parallel
    auto config = PipelineConfig()
                      .with_name("MultiProducerMultiConsumerAsync")
                      .with_compute_threads(NUM_PRODUCERS + NUM_CONSUMERS);

    Pipeline pipeline(config);

    // Combine all producers and consumers as source tasks
    std::vector<std::shared_ptr<Task>> all_tasks;
    all_tasks.insert(all_tasks.end(), producers.begin(), producers.end());
    all_tasks.insert(all_tasks.end(), consumers.begin(), consumers.end());

    pipeline.set_source(all_tasks);

    auto output = pipeline.execute();

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
// adopt_producer() and register_producer() Tests
// ============================================================================

TEST_CASE("Channel - adopt_producer() basic") {
    Channel<int> channel(10);

    CHECK(channel.num_producers() == 0);

    // Pre-register, then adopt
    channel.register_producer();
    CHECK(channel.num_producers() == 1);

    {
        auto guard = channel.adopt_producer();  // no increment
        CHECK(channel.num_producers() == 1);    // still 1
    }
    // guard destroyed -> released
    CHECK(channel.num_producers() == 0);
    CHECK(channel.is_closed() == true);
}

TEST_CASE("Channel - register_producer() bulk") {
    Channel<int> channel(10);

    channel.register_producers(5);
    CHECK(channel.num_producers() == 5);

    // Adopt and release one at a time
    for (int i = 0; i < 5; ++i) {
        auto guard = channel.adopt_producer();
        CHECK(channel.num_producers() == static_cast<std::size_t>(5 - i));
    }
    // All released
    CHECK(channel.num_producers() == 0);
    CHECK(channel.is_closed() == true);
}

TEST_CASE("Channel - register_producers(0) is a no-op") {
    Channel<int> channel(10);

    channel.register_producers(0);
    CHECK(channel.num_producers() == 0);
}

TEST_CASE("Channel - adopt_producer() with threads") {
    Channel<int> channel(100);

    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 250;
    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};

    // Pre-register all producers
    channel.register_producers(NUM_PRODUCERS);

    // Producer threads use adopt_producer()
    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            auto guard = channel.adopt_producer();  // RAII release
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                int value = p * 1000 + i;
                CHECK(blocking_send(channel, value));
                total_produced.fetch_add(value);
            }
        });
    }

    // Consumer thread
    std::thread consumer([&]() {
        int value;
        while (blocking_receive(channel, value)) {
            total_consumed.fetch_add(value);
        }
    });

    for (auto& t : producers) t.join();
    consumer.join();

    CHECK(total_produced.load() == total_consumed.load());
}

TEST_CASE("Channel - adopt_producer() with scope.spawn() pattern") {
    auto channel = coro::make_channel<int>(100);

    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 50;
    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};

    auto streaming_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
                // Pre-register before spawning
                channel->register_producers(NUM_PRODUCERS);

                // Spawn producers that adopt pre-registered slots
                for (int p = 0; p < NUM_PRODUCERS; ++p) {
                    scope.spawn(
                        [&, p](CoroScope& /*pctx*/) -> coro::CoroTask<void> {
                            auto guard = channel->adopt_producer();
                            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                                int value = p * 1000 + i;
                                CHECK(co_await channel->send(value));
                                total_produced.fetch_add(value);
                            }
                            co_return;
                        });
                }

                // Consumer coroutine
                scope.spawn([&](CoroScope& cctx) -> coro::CoroTask<void> {
                    while (auto item = co_await cctx.receive(channel)) {
                        total_consumed.fetch_add(*item);
                    }
                    co_return;
                });

                co_return;
            });
            co_return;
        },
        "ScopeSpawnAdopt");

    auto config = PipelineConfig()
                      .with_name("AdoptScopeSpawn")
                      .with_compute_threads(NUM_PRODUCERS + 1);

    Pipeline pipeline(config);
    pipeline.set_source(streaming_task);
    pipeline.set_destination(streaming_task);
    pipeline.execute();

    CHECK(total_produced.load() == total_consumed.load());
    int expected_sum = 0;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
            expected_sum += p * 1000 + i;
        }
    }
    CHECK(total_produced.load() == expected_sum);
}

TEST_CASE("Channel - adopt_producer() early exit in scope.spawn()") {
    auto channel = coro::make_channel<int>(100);

    constexpr int NUM_PRODUCERS = 4;
    std::atomic<int> total_consumed{0};

    auto streaming_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
                channel->register_producers(NUM_PRODUCERS);

                // Some producers exit early (simulating skip/error)
                for (int p = 0; p < NUM_PRODUCERS; ++p) {
                    scope.spawn(
                        [&, p](CoroScope& /*pctx*/) -> coro::CoroTask<void> {
                            auto guard = channel->adopt_producer();
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
                scope.spawn([&](CoroScope& cctx) -> coro::CoroTask<void> {
                    while (auto item = co_await cctx.receive(channel)) {
                        total_consumed.fetch_add(1);
                    }
                    co_return;
                });

                co_return;
            });
            co_return;
        },
        "EarlyExitAdopt");

    auto config = PipelineConfig()
                      .with_name("AdoptEarlyExit")
                      .with_compute_threads(NUM_PRODUCERS + 1);

    Pipeline pipeline(config);
    pipeline.set_source(streaming_task);
    pipeline.set_destination(streaming_task);
    pipeline.execute();

    // Only producers 1 and 3 send 10 items each
    CHECK(total_consumed.load() == 20);
}

TEST_CASE("Channel - adopt_producer() two-stage pipeline with scope.spawn()") {
    auto stage1 = coro::make_channel<int>(50);
    auto stage2 = coro::make_channel<int>(50);

    constexpr int NUM_PRODUCERS = 3;
    constexpr int NUM_WORKERS = 2;
    constexpr int ITEMS_PER_PRODUCER = 20;
    std::atomic<int> total_consumed{0};

    auto streaming_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
                // Stage 1: producers -> stage1 channel
                stage1->register_producers(NUM_PRODUCERS);
                for (int p = 0; p < NUM_PRODUCERS; ++p) {
                    scope.spawn(
                        [&, p](CoroScope& /*pctx*/) -> coro::CoroTask<void> {
                            auto guard = stage1->adopt_producer();
                            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                                CHECK(co_await stage1->send(p * 1000 + i));
                            }
                            co_return;
                        });
                }

                // Stage 2: workers read stage1, write stage2
                stage2->register_producers(NUM_WORKERS);
                for (int w = 0; w < NUM_WORKERS; ++w) {
                    scope.spawn([&](CoroScope& wctx) -> coro::CoroTask<void> {
                        auto guard = stage2->adopt_producer();
                        while (auto item = co_await wctx.receive(stage1)) {
                            CHECK(co_await stage2->send(*item * 2));
                        }
                        co_return;
                    });
                }

                // Final consumer
                scope.spawn([&](CoroScope& cctx) -> coro::CoroTask<void> {
                    while (auto item = co_await cctx.receive(stage2)) {
                        total_consumed.fetch_add(1);
                    }
                    co_return;
                });

                co_return;
            });
            co_return;
        },
        "TwoStagePipeline");

    auto config = PipelineConfig()
                      .with_name("AdoptTwoStage")
                      .with_compute_threads(NUM_PRODUCERS + NUM_WORKERS + 1);

    Pipeline pipeline(config);
    pipeline.set_source(streaming_task);
    pipeline.set_destination(streaming_task);
    pipeline.execute();

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

    auto streaming_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
                // Single producer
                scope.spawn_producer(
                    input, [](CoroScope& /*ctx*/) -> coro::Generator<int> {
                        for (int i = 1; i <= NUM_ITEMS; ++i) {
                            co_yield i;
                        }
                    });

                // Transform: square each value
                scope.spawn_transforms(
                    input, output, 4,
                    [](CoroScope& /*ctx*/, int val) -> coro::CoroTask<int> {
                        co_return val* val;
                    });

                // Consumer
                scope.spawn_consumers(
                    output, 1,
                    [&](CoroScope& /*ctx*/, int val) -> coro::CoroTask<void> {
                        total_consumed.fetch_add(val);
                        co_return;
                    });

                co_return;
            });
            co_return;
        },
        "TransformBasic");

    auto config =
        PipelineConfig().with_name("SpawnTransforms").with_compute_threads(4);

    Pipeline pipeline(config);
    pipeline.set_source(streaming_task);
    pipeline.set_destination(streaming_task);
    pipeline.execute();

    // sum of squares 1..100
    int expected = 0;
    for (int i = 1; i <= NUM_ITEMS; ++i) expected += i * i;
    CHECK(total_consumed.load() == expected);
}

TEST_CASE("Channel - spawn_transforms() two-stage pipeline") {
    auto stage1 = coro::make_channel<int>(50);
    auto stage2 = coro::make_channel<std::string>(50);
    auto stage3 = coro::make_channel<std::string>(50);

    std::atomic<int> total_consumed{0};

    auto streaming_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
                // Producer: emit integers
                scope.spawn_producer(
                    stage1, [](CoroScope& /*ctx*/) -> coro::Generator<int> {
                        for (int i = 0; i < 50; ++i) co_yield i;
                    });

                // Transform 1: int -> string
                scope.spawn_transforms(
                    stage1, stage2, 2,
                    [](CoroScope& /*ctx*/,
                       int val) -> coro::CoroTask<std::string> {
                        co_return std::to_string(val);
                    });

                // Transform 2: string -> string (prefix)
                scope.spawn_transforms(
                    stage2, stage3, 2,
                    [](CoroScope& /*ctx*/,
                       std::string val) -> coro::CoroTask<std::string> {
                        co_return "item_" + val;
                    });

                // Consumer
                scope.spawn_consumers(
                    stage3, 1,
                    [&](CoroScope& /*ctx*/,
                        std::string /*val*/) -> coro::CoroTask<void> {
                        total_consumed.fetch_add(1);
                        co_return;
                    });

                co_return;
            });
            co_return;
        },
        "TwoStageTransform");

    auto config =
        PipelineConfig().with_name("ChainedTransforms").with_compute_threads(8);

    Pipeline pipeline(config);
    pipeline.set_source(streaming_task);
    pipeline.set_destination(streaming_task);
    pipeline.execute();

    CHECK(total_consumed.load() == 50);
}
