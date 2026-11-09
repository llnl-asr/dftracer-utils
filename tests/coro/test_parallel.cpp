// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/parallel.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace std::chrono_literals;

TEST_CASE("parallel_for covers every index once in each partition") {
    Runtime runtime(4);
    for (const Partition partition : {Partition::Dynamic, Partition::Static}) {
        for (const std::int64_t size : {0, 1, 7, 1000, 1003}) {
            dstd::Vector<std::atomic<int>> hits(static_cast<std::size_t>(size));
            ParallelForOptions options;
            options.grain = 16;
            options.partition = partition;
            dftu_utils_test::ok(runtime.block_on(parallel_for(
                size,
                [&hits](std::int64_t begin, std::int64_t end) {
                    for (std::int64_t i = begin; i < end; ++i) {
                        hits[static_cast<std::size_t>(i)].fetch_add(1);
                    }
                },
                options)));
            for (auto& hit : hits) CHECK(hit.load() == 1);
        }
    }
}

TEST_CASE("a static partition gives the same chunks on each run") {
    Runtime runtime(4);
    auto chunks_of = [&runtime]() {
        std::mutex lock;
        dstd::Vector<dstd::Pair<std::int64_t, std::int64_t>> seen;
        ParallelForOptions options;
        options.max_in_flight = 3;
        options.partition = Partition::Static;
        dftu_utils_test::ok(runtime.block_on(parallel_for(
            1000,
            [&](std::int64_t begin, std::int64_t end) {
                const std::lock_guard<std::mutex> guard(lock);
                seen.emplace_back(begin, end);
            },
            options)));
        std::sort(seen.begin(), seen.end());
        return seen;
    };
    const auto first = chunks_of();
    REQUIRE(first.size() == 3);
    CHECK(first[0].second - first[0].first == 334);
    CHECK(first[1].second - first[1].first == 333);
    CHECK(first == chunks_of());
}

TEST_CASE("parallel_for raises the first error") {
    Runtime runtime(2);
    dstd::Condition condition = dstd::Condition::Unknown;
    try {
        dftu_utils_test::ok(
            runtime.block_on(parallel_for(100, [](std::int64_t, std::int64_t) {
                throw common::DFTUtilsException(
                    dstd::Error{dstd::Condition::Io, "planned failure"});
            })));
    } catch (const common::DFTUtilsException& e) {
        condition = e.condition();
    }
    CHECK(condition == dstd::Condition::Io);
}

TEST_CASE("a cancelled parallel_for starts no more parts and reports it") {
    Runtime runtime(2);
    std::atomic<int> parts{0};
    ParallelForOptions options;
    options.max_in_flight = 1;
    auto result =
        runtime.block_on(with_timeout(parallel_for(
                                          1000,
                                          [&parts](std::int64_t, std::int64_t) {
                                              parts.fetch_add(1);
                                              std::this_thread::sleep_for(1ms);
                                          },
                                          options),
                                      20ms));
    REQUIRE_FALSE(result);
    CHECK(result.error().condition() == dstd::Condition::Timeout);
    CHECK(parts.load() < 1000);
}

TEST_CASE("parallel_reduce keeps range order for a non-commutative combine") {
    Runtime runtime(4);
    for (const Partition partition : {Partition::Dynamic, Partition::Static}) {
        ParallelForOptions options;
        options.grain = 3;
        options.partition = partition;
        const dstd::String joined =
            dftu_utils_test::ok(runtime.block_on(parallel_reduce(
                26, dstd::String{},
                [](std::int64_t begin, std::int64_t end) {
                    dstd::String part;
                    for (std::int64_t i = begin; i < end; ++i) {
                        part.push_back(static_cast<char>('a' + i));
                    }
                    return part;
                },
                [](dstd::String left, dstd::String right) {
                    left += right;
                    return left;
                },
                options)));
        CHECK(joined == "abcdefghijklmnopqrstuvwxyz");
    }
    CHECK(dftu_utils_test::ok(runtime.block_on(parallel_reduce(
              0, 5, [](std::int64_t, std::int64_t) { return 1; },
              [](int a, int b) { return a + b; }))) == 5);
}

TEST_CASE("parallel_scan matches a serial prefix sum") {
    Runtime runtime(4);
    constexpr std::int64_t SIZE = 10000;
    dstd::Vector<std::int64_t> input(SIZE);
    for (std::int64_t i = 0; i < SIZE; ++i) {
        input[static_cast<std::size_t>(i)] = (i * 7) % 13;
    }
    dstd::Vector<std::int64_t> serial(SIZE);
    std::int64_t running = 0;
    for (std::int64_t i = 0; i < SIZE; ++i) {
        running += input[static_cast<std::size_t>(i)];
        serial[static_cast<std::size_t>(i)] = running;
    }
    for (const Partition partition : {Partition::Dynamic, Partition::Static}) {
        dstd::Vector<std::int64_t> output(SIZE);
        ParallelForOptions options;
        options.grain = 64;
        options.partition = partition;
        const std::int64_t total =
            dftu_utils_test::ok(runtime.block_on(parallel_scan(
                SIZE, std::int64_t{0},
                [&](std::int64_t begin, std::int64_t end, std::int64_t start,
                    bool is_final) {
                    std::int64_t sum = start;
                    for (std::int64_t i = begin; i < end; ++i) {
                        sum += input[static_cast<std::size_t>(i)];
                        if (is_final) output[static_cast<std::size_t>(i)] = sum;
                    }
                    return sum;
                },
                [](std::int64_t a, std::int64_t b) { return a + b; },
                options)));
        CHECK(total == running);
        CHECK(output == serial);
    }
}
