#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/task.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;

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
    CHECK(channel.send_blocking(42) == true);
    CHECK(channel.send_blocking(100) == true);
    CHECK(channel.size() == 2);

    // Receive items
    int value;
    CHECK(channel.receive(value) == true);
    CHECK(value == 42);

    CHECK(channel.receive(value) == true);
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

    channel.send_blocking(42);
    channel.close();

    CHECK(channel.is_closed() == true);

    // Can still receive existing items
    int value;
    CHECK(channel.receive(value) == true);
    CHECK(value == 42);

    // Cannot send after close
    CHECK(channel.send_blocking(100) == false);
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
            if (channel.send_blocking(i)) sum_produced.fetch_add(i);
        }
    });

    // Consumer thread
    std::thread consumer([&]() {
        int value;
        while (channel.receive(value)) {
            sum_consumed.fetch_add(value);
        }
    });

    producer.join();
    consumer.join();

    CHECK(sum_produced.load() == sum_consumed.load());
}

TEST_CASE("Channel - Multiple producers, single consumer") {
    Channel<int> channel(100);

    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 250;
    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};

    // Producer threads
    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            auto guard = channel.producer_guard();
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                int value = p * 1000 + i;
                channel.send_blocking(value);
                total_produced.fetch_add(value);
            }
        });
    }

    // Consumer thread
    std::thread consumer([&]() {
        int value;
        while (channel.receive(value)) {
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
            channel.send_blocking(i);
            sum_produced.fetch_add(i);
        }
    });

    // Consumer threads
    std::vector<std::thread> consumers;
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&]() {
            int value;
            while (channel.receive(value)) {
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

    channel.send_blocking("Hello");
    channel.send_blocking("World");
    channel.send_blocking("from");
    channel.send_blocking("Channel");

    std::string msg;
    CHECK(channel.receive(msg) == true);
    CHECK(msg == "Hello");

    CHECK(channel.receive(msg) == true);
    CHECK(msg == "World");

    CHECK(channel.receive(msg) == true);
    CHECK(msg == "from");

    CHECK(channel.receive(msg) == true);
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

    channel.send_blocking(Event{1, "start", 0.0});
    channel.send_blocking(Event{2, "process", 42.5});
    channel.send_blocking(Event{3, "end", 100.0});

    Event evt;
    CHECK(channel.receive(evt) == true);
    CHECK(evt.id == 1);
    CHECK(evt.name == "start");

    CHECK(channel.receive(evt) == true);
    CHECK(evt.id == 2);
    CHECK(evt.value == 42.5);

    CHECK(channel.receive(evt) == true);
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
        [&]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
            auto guard = channel.producer_guard();

            for (int i = 0; i < 100; ++i) {
                channel.send_blocking(i);
                producer_sum.fetch_add(i);
            }

            co_return 100;
        },
        "Producer");

    // Create consumer task
    auto consumer_task = make_task(
        [&]([[maybe_unused]] TaskContext& ctx,
            [[maybe_unused]] int count) -> coro::CoroTask<int> {
            int value;
            int items_consumed = 0;

            while (channel.receive(value)) {
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
        [&]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
            auto guard = input_channel.producer_guard();

            for (int i = 1; i <= 10; ++i) {
                input_channel.send_blocking(i);
                produced_values.push_back(i);
            }

            co_return 10;
        },
        "Producer");

    // Transform task (multiply by 2)
    auto transform = make_task(
        [&]([[maybe_unused]] TaskContext& ctx,
            [[maybe_unused]] int count) -> coro::CoroTask<int> {
            auto out_guard = output_channel.producer_guard();

            int value;
            while (input_channel.receive(value)) {
                int transformed = value * 2;
                output_channel.send_blocking(transformed);
                transformed_values.push_back(transformed);
            }

            co_return static_cast<int>(transformed_values.size());
        },
        "Transform");

    // Consumer task
    auto consumer = make_task(
        [&]([[maybe_unused]] TaskContext& ctx,
            [[maybe_unused]] int count) -> coro::CoroTask<int> {
            int value;
            while (output_channel.receive(value)) {
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
        [&]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
            auto guard = channel.producer_guard();

            for (int i = 0; i < 100; ++i) {
                channel.send_blocking(i);
                sum_produced.fetch_add(i);
            }

            co_return 100;
        },
        "Producer");

    // Consumer 1 - no input parameter since it's a source task
    auto consumer1 = make_task(
        [&]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
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
        [&]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
            int value;
            int items = 0;

            while (channel.receive(value)) {
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
            channel.send_blocking(i);
            items_sent.fetch_add(1);
        }
    });

    std::thread consumer([&]() {
        int value;
        while (channel.receive(value)) {
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
            channel.send_blocking(i);
        }

        int value;
        int count = 0;
        while (channel.try_receive(value)) {
            count++;
        }

        CHECK(count == 10);
    }
}

// ============================================================================
// Async I/O with receive_async() Tests
// ============================================================================

TEST_CASE("Channel - receive_async() with I/O executor") {
    Channel<int> channel(100);

    std::atomic<int> producer_sum{0};
    std::atomic<int> consumer_sum{0};

    // Producer task
    auto producer = make_task(
        [&]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
            auto guard = channel.producer_guard();

            for (int i = 0; i < 50; ++i) {
                channel.send_blocking(i);
                producer_sum.fetch_add(i);
            }

            co_return 50;
        },
        "Producer");

    // Consumer task using receive_async()
    auto consumer = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<int> {
            int items_consumed = 0;

            while (auto item_opt = co_await ctx.receive_async(channel)) {
                consumer_sum.fetch_add(*item_opt);
                items_consumed++;
            }

            co_return items_consumed;
        },
        "Consumer");

    // No dependencies - producer and consumer run in parallel

    // Execute with I/O executor enabled
    auto config = PipelineConfig()
                      .with_name("AsyncReceiveTest")
                      .with_compute_threads(2)
                      .with_io_threads(2);  // Enable I/O executor

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
        [&]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
            auto guard = channel.producer_guard();

            for (int i = 0; i < 100; ++i) {
                channel.send_blocking(i);
                producer_sum.fetch_add(i);
            }

            co_return 100;
        },
        "Producer");

    // Consumer 1 using receive_async()
    auto consumer1 = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<int> {
            int items = 0;

            while (auto item_opt = co_await ctx.receive_async(channel)) {
                consumer1_sum.fetch_add(*item_opt);
                items++;
            }

            co_return items;
        },
        "Consumer1");

    // Consumer 2 using receive_async()
    auto consumer2 = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<int> {
            int items = 0;

            while (auto item_opt = co_await ctx.receive_async(channel)) {
                consumer2_sum.fetch_add(*item_opt);
                items++;
            }

            co_return items;
        },
        "Consumer2");

    // Execute with I/O executor
    auto config = PipelineConfig()
                      .with_name("MultiConsumerAsync")
                      .with_compute_threads(3)
                      .with_io_threads(2);

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
        [&]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
            auto guard = input_channel.producer_guard();

            for (int i = 1; i <= 20; ++i) {
                input_channel.send_blocking(i);
                produced_values.push_back(i);
            }

            co_return 20;
        },
        "Producer");

    // Transform task using receive_async() (square the values)
    auto transform = make_task(
        [&](TaskContext& ctx,
            [[maybe_unused]] int count) -> coro::CoroTask<int> {
            auto out_guard = output_channel.producer_guard();

            while (auto item_opt = co_await ctx.receive_async(input_channel)) {
                int transformed = (*item_opt) * (*item_opt);  // Square
                output_channel.send_blocking(transformed);
                transformed_values.push_back(transformed);
            }

            co_return static_cast<int>(transformed_values.size());
        },
        "Transform");

    // Consumer task using receive_async()
    auto consumer = make_task(
        [&](TaskContext& ctx,
            [[maybe_unused]] int count) -> coro::CoroTask<int> {
            while (auto item_opt = co_await ctx.receive_async(output_channel)) {
                consumed_values.push_back(*item_opt);
            }

            co_return static_cast<int>(consumed_values.size());
        },
        "Consumer");

    transform->depends_on(producer);
    consumer->depends_on(transform);

    // Execute with I/O executor
    auto config = PipelineConfig()
                      .with_name("AsyncTransformPipeline")
                      .with_compute_threads(3)
                      .with_io_threads(2);

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

TEST_CASE("Channel - receive_async() without I/O executor (fallback)") {
    Channel<int> channel(50);

    std::atomic<int> producer_sum{0};
    std::atomic<int> consumer_sum{0};

    // Producer task
    auto producer = make_task(
        [&]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
            auto guard = channel.producer_guard();

            for (int i = 0; i < 30; ++i) {
                channel.send_blocking(i);
                producer_sum.fetch_add(i);
            }

            co_return 30;
        },
        "Producer");

    // Consumer task using receive_async() (should work without I/O executor)
    auto consumer = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<int> {
            int items_consumed = 0;

            while (auto item_opt = co_await ctx.receive_async(channel)) {
                consumer_sum.fetch_add(*item_opt);
                items_consumed++;
            }

            co_return items_consumed;
        },
        "Consumer");

    // No dependencies - producer and consumer run in parallel

    // Execute WITHOUT I/O executor (io_threads = 0)
    auto config = PipelineConfig()
                      .with_name("AsyncReceiveFallback")
                      .with_compute_threads(2)
                      .with_io_threads(0);  // No I/O executor

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
        [&]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
            auto guard = channel.producer_guard();

            for (int i = 0; i < NUM_ITEMS; ++i) {
                channel.send_blocking(i);
                items_sent.fetch_add(1);
            }

            co_return NUM_ITEMS;
        },
        "Producer");

    // Consumer task using receive_async()
    auto consumer = make_task(
        [&](TaskContext& ctx) -> coro::CoroTask<int> {
            int items = 0;

            while (auto item_opt = co_await ctx.receive_async(channel)) {
                items_received.fetch_add(1);
                items++;
            }

            co_return items;
        },
        "Consumer");

    // No dependencies - producer and consumer run in parallel

    // Execute with I/O executor
    auto config = PipelineConfig()
                      .with_name("AsyncReceiveStress")
                      .with_compute_threads(2)
                      .with_io_threads(4);

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

    // Create multiple producers
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        auto producer = make_task(
            [&, p]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
                auto guard = channel.producer_guard();

                for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                    int value = p * 1000 + i;
                    channel.send_blocking(value);
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
            [&]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
                int items = 0;
                int value;

                while (channel.receive(value)) {
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

    // Create multiple producers
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        auto producer = make_task(
            [&, p]([[maybe_unused]] TaskContext& ctx) -> coro::CoroTask<int> {
                auto guard = channel.producer_guard();

                for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                    int value = p * 1000 + i;
                    channel.send_blocking(value);
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
            [&](TaskContext& ctx) -> coro::CoroTask<int> {
                int items = 0;

                while (auto item_opt = co_await ctx.receive_async(channel)) {
                    total_consumed.fetch_add(*item_opt);
                    items++;
                }

                co_return items;
            },
            "Consumer" + std::to_string(c));
        consumers.push_back(consumer);
    }

    // Execute all tasks in parallel with I/O executor
    auto config = PipelineConfig()
                      .with_name("MultiProducerMultiConsumerAsync")
                      .with_compute_threads(NUM_PRODUCERS + NUM_CONSUMERS)
                      .with_io_threads(4);

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
