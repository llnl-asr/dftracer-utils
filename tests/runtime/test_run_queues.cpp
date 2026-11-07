// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/scheduler/run_queues.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

using namespace dftracer::utils;
using runtime::scheduler::InjectQueue;
using runtime::scheduler::LocalQueue;

namespace {

void* token(std::size_t value) { return reinterpret_cast<void*>(value + 1); }
std::size_t value_of(void* task) {
    return reinterpret_cast<std::size_t>(task) - 1;
}

}  // namespace

TEST_SUITE("RunQueues") {
    TEST_CASE("the inject queue keeps order across growth and wrap") {
        InjectQueue queue;
        void* out[64];
        std::size_t next_out = 0;
        std::size_t next_in = 0;
        for (int round = 0; round < 40; ++round) {
            for (int i = 0; i < 100; ++i) queue.push(token(next_in++));
            std::size_t count = 0;
            while (queue.size_approx() > 10 * static_cast<std::size_t>(round) &&
                   (count = queue.pop_batch(out, 64)) != 0) {
                for (std::size_t i = 0; i < count; ++i) {
                    REQUIRE(value_of(out[i]) == next_out++);
                }
            }
        }
        while (const std::size_t count = queue.pop_batch(out, 64)) {
            for (std::size_t i = 0; i < count; ++i) {
                REQUIRE(value_of(out[i]) == next_out++);
            }
        }
        CHECK(next_out == next_in);
        CHECK(queue.size_approx() == 0);
        CHECK(queue.pop_batch(out, 64) == 0);
    }

    TEST_CASE(
        "a local queue is FIFO and a full one spills half with the task") {
        InjectQueue inject;
        auto queue = dstd::make_unique<LocalQueue>();
        CHECK(queue->pop() == nullptr);
        for (std::size_t i = 0; i < LocalQueue::CAPACITY; ++i) {
            queue->push_back(token(i), inject);
        }
        CHECK(queue->size_approx() == LocalQueue::CAPACITY);
        CHECK(inject.size_approx() == 0);

        queue->push_back(token(LocalQueue::CAPACITY), inject);
        CHECK(queue->size_approx() == LocalQueue::CAPACITY / 2);
        CHECK(inject.size_approx() == LocalQueue::CAPACITY / 2 + 1);

        for (std::size_t i = LocalQueue::CAPACITY / 2; i < LocalQueue::CAPACITY;
             ++i) {
            REQUIRE(value_of(queue->pop()) == i);
        }
        CHECK(queue->pop() == nullptr);

        void* out[LocalQueue::CAPACITY];
        REQUIRE(inject.pop_batch(out, LocalQueue::CAPACITY) ==
                LocalQueue::CAPACITY / 2 + 1);
        for (std::size_t i = 0; i < LocalQueue::CAPACITY / 2; ++i) {
            REQUIRE(value_of(out[i]) == i);
        }
        CHECK(value_of(out[LocalQueue::CAPACITY / 2]) == LocalQueue::CAPACITY);
    }

    TEST_CASE("a steal takes half of the victim, the oldest first") {
        InjectQueue inject;
        auto victim = dstd::make_unique<LocalQueue>();
        auto thief = dstd::make_unique<LocalQueue>();
        CHECK(victim->steal_into(*thief) == nullptr);
        for (std::size_t i = 0; i < 10; ++i)
            victim->push_back(token(i), inject);

        void* first = victim->steal_into(*thief);
        REQUIRE(first != nullptr);
        CHECK(value_of(first) == 4);
        CHECK(victim->size_approx() == 5);
        CHECK(thief->size_approx() == 4);
        for (std::size_t i = 0; i < 4; ++i) {
            REQUIRE(value_of(thief->pop()) == i);
        }
        for (std::size_t i = 5; i < 10; ++i) {
            REQUIRE(value_of(victim->pop()) == i);
        }

        victim->push_back(token(0), inject);
        CHECK(value_of(victim->steal_into(*thief)) == 0);
        CHECK(thief->size_approx() == 0);
    }

    TEST_CASE("a thief with a full queue takes nothing") {
        InjectQueue inject;
        auto victim = dstd::make_unique<LocalQueue>();
        auto thief = dstd::make_unique<LocalQueue>();
        victim->push_back(token(0), inject);
        for (std::size_t i = 0; i <= LocalQueue::CAPACITY / 2; ++i) {
            thief->push_back(token(i), inject);
        }
        CHECK(victim->steal_into(*thief) == nullptr);
        CHECK(victim->size_approx() == 1);
    }

    TEST_CASE("every task runs once under an owner, thieves and overflow") {
        constexpr std::size_t TASKS = 400000;
        constexpr std::size_t THIEVES = 6;
        InjectQueue inject;
        auto owner = dstd::make_unique<LocalQueue>();
        auto seen = dstd::make_unique<std::atomic<std::uint8_t>[]>(TASKS);
        std::atomic<std::size_t> done{0};
        const auto run = [&](void* task) {
            seen[value_of(task)].fetch_add(1, std::memory_order_relaxed);
            done.fetch_add(1, std::memory_order_relaxed);
        };

        dstd::Vector<dstd::Thread> threads;
        for (std::size_t t = 0; t < THIEVES; ++t) {
            threads.emplace_back([&] {
                auto own = dstd::make_unique<LocalQueue>();
                void* batch[16];
                while (done.load(std::memory_order_relaxed) < TASKS) {
                    if (void* task = owner->steal_into(*own)) {
                        run(task);
                        while (void* next = own->pop()) run(next);
                    }
                    const std::size_t count = inject.pop_batch(batch, 16);
                    for (std::size_t i = 0; i < count; ++i) run(batch[i]);
                }
            });
        }
        for (std::size_t i = 0; i < TASKS; ++i) {
            owner->push_back(token(i), inject);
            if (i % 3 == 0) {
                if (void* task = owner->pop()) run(task);
            }
        }
        while (void* task = owner->pop()) run(task);
        for (auto& thread : threads) thread.join();

        std::size_t wrong = 0;
        for (std::size_t i = 0; i < TASKS; ++i) {
            if (seen[i].load(std::memory_order_relaxed) != 1) ++wrong;
        }
        CHECK(wrong == 0);
    }
}
