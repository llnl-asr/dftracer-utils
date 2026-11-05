// SPDX-License-Identifier: MIT
//
// Executor pool scheduling cost by thread count.
//
//   yield_storm   4x threads coroutines yield in a loop. One op is one yield.
//                 Queue churn with every worker busy.
//   spawn_join    one coroutine spawns 1000 empty children and joins them, in
//                 a loop. One op is one child. Work from one worker spreads to
//                 the others.
//   wave          4x threads coroutines each spawn one child and await it, in
//                 a loop. One op is one child. A wake on the same worker.
//   relay         one token passes around 8 coroutines through semaphores. One
//                 op is one pass. One runnable task, so every pass is a wake.
//   spread        one coroutine runs alone, then 1000 children, and sleeps so
//                 that the pool goes idle between rounds. One op is one child.
//                 Parked workers wake for a burst.
//
//   pool_bench --threads N [--reps R] [--scale F] [--case NAME]
//   pool_bench --smoke   (small sizes; checks each sum, exits nonzero on a
//   loss)
//
// Each row prints the median ns per op.
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace dftracer::utils;

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
    std::size_t threads = 4;
    std::size_t scale_percent = 100;
    int reps = 5;
    bool smoke = false;
    const char* only = nullptr;
};

struct Outcome {
    std::uint64_t ops = 0;
    std::uint64_t counted = 0;
};

std::size_t scaled(const Config& config, std::size_t n) {
    return std::max<std::size_t>(n * config.scale_percent / 100, 1);
}

coro::CoroTask<void> yielder(std::size_t rounds,
                             std::atomic<std::uint64_t>& counted) {
    for (std::size_t i = 0; i < rounds; ++i) {
        co_await coro::yield();
        counted.fetch_add(1, std::memory_order_relaxed);
    }
}

coro::CoroTask<void> empty_child(std::atomic<std::uint64_t>& counted) {
    counted.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

coro::CoroTask<void> spawn_children(std::size_t count,
                                    std::atomic<std::uint64_t>& counted) {
    co_await coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        for (std::size_t i = 0; i < count; ++i) {
            scope.spawn(empty_child(counted));
        }
        co_return;
    });
}

coro::CoroTask<void> waver(std::size_t rounds,
                           std::atomic<std::uint64_t>& counted) {
    for (std::size_t i = 0; i < rounds; ++i) {
        auto children = spawn_children(1, counted);
        co_await std::move(children);
    }
}

coro::CoroTask<void> relay_hop(coro::AsyncSemaphore& in,
                               coro::AsyncSemaphore& out, std::size_t rounds,
                               std::atomic<std::uint64_t>& counted) {
    for (std::size_t i = 0; i < rounds; ++i) {
        co_await in.acquire(1);
        counted.fetch_add(1, std::memory_order_relaxed);
        out.release(1);
    }
}

Outcome yield_storm(const Config& config, Runtime& rt) {
    const std::size_t tasks = 4 * config.threads;
    const std::size_t rounds = scaled(config, 20000);
    std::atomic<std::uint64_t> counted{0};
    rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            for (std::size_t i = 0; i < tasks; ++i) {
                scope.spawn(yielder(rounds, counted));
            }
            co_return;
        }));
    return {tasks * rounds, counted.load()};
}

Outcome spawn_join(const Config& config, Runtime& rt) {
    const std::size_t rounds = scaled(config, 2000);
    constexpr std::size_t CHILDREN = 1000;
    std::atomic<std::uint64_t> counted{0};
    auto body = [&]() -> coro::CoroTask<void> {
        for (std::size_t i = 0; i < rounds; ++i) {
            auto children = spawn_children(CHILDREN, counted);
            co_await std::move(children);
        }
    };
    rt.block_on(body());
    return {rounds * CHILDREN, counted.load()};
}

Outcome wave(const Config& config, Runtime& rt) {
    const std::size_t tasks = 4 * config.threads;
    const std::size_t rounds = scaled(config, 5000);
    std::atomic<std::uint64_t> counted{0};
    rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            for (std::size_t i = 0; i < tasks; ++i) {
                scope.spawn(waver(rounds, counted));
            }
            co_return;
        }));
    return {tasks * rounds, counted.load()};
}

Outcome relay(const Config& config, Runtime& rt) {
    constexpr std::size_t HOPS = 8;
    const std::size_t rounds = scaled(config, 50000);
    std::atomic<std::uint64_t> counted{0};
    dstd::Vector<dstd::UniquePtr<coro::AsyncSemaphore>> gates;
    for (std::size_t i = 0; i < HOPS; ++i) {
        gates.push_back(dstd::make_unique<coro::AsyncSemaphore>(1));
        // A capacity of zero would clamp every acquire to nothing.
        if (i != 0 && !gates.back()->try_acquire(1)) return {};
    }
    rt.block_on(
        coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
            for (std::size_t i = 0; i < HOPS; ++i) {
                scope.spawn(relay_hop(*gates[i], *gates[(i + 1) % HOPS], rounds,
                                      counted));
            }
            co_return;
        }));
    return {HOPS * rounds, counted.load()};
}

Outcome spread(const Config& config, Runtime& rt) {
    const std::size_t rounds = scaled(config, 200);
    constexpr std::size_t CHILDREN = 1000;
    std::atomic<std::uint64_t> counted{0};
    for (std::size_t i = 0; i < rounds; ++i) {
        rt.block_on(spawn_children(CHILDREN, counted));
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return {rounds * CHILDREN, counted.load()};
}

struct Case {
    const char* name;
    Outcome (*run)(const Config&, Runtime&);
};

constexpr Case CASES[] = {{"yield_storm", yield_storm},
                          {"spawn_join", spawn_join},
                          {"wave", wave},
                          {"relay", relay},
                          {"spread", spread}};

}  // namespace

int main(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const dstd::StringView arg = argv[i];
        if (arg == "--smoke") {
            config.smoke = true;
            config.reps = 1;
            config.scale_percent = 2;
        } else if (arg == "--threads" && i + 1 < argc) {
            config.threads = static_cast<std::size_t>(std::atoi(argv[++i]));
        } else if (arg == "--reps" && i + 1 < argc) {
            config.reps = std::atoi(argv[++i]);
        } else if (arg == "--scale" && i + 1 < argc) {
            config.scale_percent =
                static_cast<std::size_t>(std::atoi(argv[++i]));
        } else if (arg == "--case" && i + 1 < argc) {
            config.only = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    int failures = 0;
    for (const Case& item : CASES) {
        if (config.only != nullptr &&
            std::strcmp(config.only, item.name) != 0) {
            continue;
        }
        dstd::Vector<double> times;
        for (int rep = 0; rep < config.reps; ++rep) {
            Runtime rt(ExecutorConfig{.thread_count = config.threads}, false);
            const auto start = Clock::now();
            const Outcome outcome = item.run(config, rt);
            const std::chrono::duration<double, std::nano> took =
                Clock::now() - start;
            if (outcome.ops != outcome.counted) {
                std::fprintf(stderr, "%s: %llu ops, counted %llu\n", item.name,
                             static_cast<unsigned long long>(outcome.ops),
                             static_cast<unsigned long long>(outcome.counted));
                ++failures;
            }
            times.push_back(took.count() / static_cast<double>(outcome.ops));
        }
        std::sort(times.begin(), times.end());
        std::printf("%-12s threads=%zu ns_per_op=%.1f\n", item.name,
                    config.threads, times[times.size() / 2]);
        std::fflush(stdout);
    }
    return failures == 0 ? 0 : 1;
}
