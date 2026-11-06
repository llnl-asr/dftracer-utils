// SPDX-License-Identifier: MIT
//
// Coroutine channel throughput by producer count, consumer count and capacity.
//
//   channel_bench                 (2M items per case, 5 reps, median)
//   channel_bench --threads N     (worker threads, default hardware)
//   channel_bench --items N --reps N
//   channel_bench --smoke         (small sizes; checks the sum, exits nonzero
//                                  when an item is lost)
//
// Each row prints ns per item and instructions per item where the counter
// exists. Every case checks that the consumers saw each item once.
#include <bench_counters.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dftracer::utils;

namespace {

struct Case {
    const char* name;
    std::size_t producers;
    std::size_t consumers;
    /// 0 means an unbounded channel.
    std::size_t capacity;
};

constexpr Case CASES[] = {
    {"spsc_cap1", 1, 1, 1},
    {"spsc_cap64", 1, 1, 64},
    {"spsc_cap4096", 1, 1, 4096},
    {"mpsc_8_cap256", 8, 1, 256},
    {"spmc_8_cap256", 1, 8, 256},
    {"mpmc_4_cap64", 4, 4, 64},
    {"mpmc_16_cap1024", 16, 16, 1024},
    {"unb_spsc", 1, 1, 0},
    {"unb_mpsc_8", 8, 1, 0},
    {"unb_spmc_8", 1, 8, 0},
    {"unb_mpmc_4", 4, 4, 0},
    {"unb_mpmc_16", 16, 16, 0},
};

template <typename Channel, typename Producer>
coro::CoroTask<void> produce(Channel& channel, Producer producer,
                             std::uint64_t first, std::uint64_t count) {
    auto guard = producer.guard();
    for (std::uint64_t i = 0; i < count; ++i) {
        const bool sent = co_await channel.send(first + i);
        if (!sent) break;
    }
}

template <typename Channel>
coro::CoroTask<void> consume(Channel& channel, std::atomic<std::uint64_t>& sum,
                             std::atomic<std::uint64_t>& seen) {
    std::uint64_t local_sum = 0;
    std::uint64_t local_seen = 0;
    for (;;) {
        auto item = co_await channel.receive();
        if (!item) break;
        local_sum += *item;
        ++local_seen;
    }
    sum.fetch_add(local_sum, std::memory_order_relaxed);
    seen.fetch_add(local_seen, std::memory_order_relaxed);
}

template <typename Channel>
coro::CoroTask<void> run_on(Channel& channel, const Case& c,
                            std::uint64_t items,
                            std::atomic<std::uint64_t>& sum,
                            std::atomic<std::uint64_t>& seen) {
    coro::Scope scope;
    const std::uint64_t per_producer = items / c.producers;
    for (std::size_t p = 0; p < c.producers; ++p) {
        scope.spawn(produce(channel, channel.producer(), p * per_producer + 1,
                            per_producer));
    }
    for (std::size_t r = 0; r < c.consumers; ++r) {
        scope.spawn(consume(channel, sum, seen));
    }
    co_await scope.join();
}

coro::CoroTask<void> run_case(const Case& c, std::uint64_t items,
                              std::atomic<std::uint64_t>& sum,
                              std::atomic<std::uint64_t>& seen) {
    if (c.capacity == 0) {
        coro::UnboundedChannel<std::uint64_t> channel;
        co_await run_on(channel, c, items, sum, seen);
    } else {
        coro::Channel<std::uint64_t> channel(c.capacity);
        co_await run_on(channel, c, items, sum, seen);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t items = 2000000;
    int reps = 5;
    std::size_t threads = 0;
    bool smoke = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            smoke = true;
        } else if (std::strcmp(argv[i], "--items") == 0 && i + 1 < argc) {
            items = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            reps = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads = std::strtoull(argv[++i], nullptr, 10);
        }
    }
    if (smoke) {
        items = 20000;
        reps = 1;
    }
    dftu::bench::init_counters();
    Runtime runtime(threads);
    std::printf("%-18s %10s %12s\n", "case", "ns/item", "instr/item");
    for (const Case& c : CASES) {
        const std::uint64_t per_producer = items / c.producers;
        const std::uint64_t total = per_producer * c.producers;
        const std::uint64_t expected = total * (total + 1) / 2;
        dstd::Vector<double> times;
        dstd::Vector<double> instructions;
        for (int rep = 0; rep < reps; ++rep) {
            std::atomic<std::uint64_t> sum{0};
            std::atomic<std::uint64_t> seen{0};
            const std::uint64_t instr_start =
                dftu::bench::instructions_retired();
            const auto start = std::chrono::steady_clock::now();
            runtime.block_on(run_case(c, items, sum, seen));
            const auto stop = std::chrono::steady_clock::now();
            const std::uint64_t instr_stop =
                dftu::bench::instructions_retired();
            if (sum.load() != expected || seen.load() != total) {
                std::fprintf(stderr, "%s: lost items: seen %llu of %llu\n",
                             c.name,
                             static_cast<unsigned long long>(seen.load()),
                             static_cast<unsigned long long>(total));
                return 1;
            }
            times.push_back(
                std::chrono::duration<double, std::nano>(stop - start).count() /
                static_cast<double>(total));
            instructions.push_back(
                static_cast<double>(instr_stop - instr_start) /
                static_cast<double>(total));
        }
        std::sort(times.begin(), times.end());
        std::sort(instructions.begin(), instructions.end());
        std::printf("%-18s %10.1f %12.1f\n", c.name, times[times.size() / 2],
                    instructions[instructions.size() / 2]);
        std::fflush(stdout);
    }
    return 0;
}
