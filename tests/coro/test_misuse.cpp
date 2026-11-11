// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/combinators.h>
#include <dftracer/utils/coro/generator.h>
#include <dftracer/utils/coro/parallel.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/stage.h>
#include <dftracer/utils/coro/stream.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <task_result.h>

#include <cstddef>
#include <utility>

using namespace dftracer::utils;

// Each misuse here ends the process. A skipped case runs it in a child process
// and a driver case checks the abort and the message.

namespace {

coro::CoroTask<void> noop() { co_return; }

coro::AsyncGenerator<int> numbers(int count) {
    for (int i = 0; i < count; ++i) co_yield i;
}

coro::Generator<int> sync_numbers(int count) {
    for (int i = 0; i < count; ++i) co_yield i;
}

template <typename T>
coro::CoroTask<void> drain(coro::AsyncGenerator<T> source) {
    for (;;) {
        auto value = co_await source.next();
        if (!value) break;
    }
}

}  // namespace

TEST_CASE("spawn after the join" * doctest::skip()) {
    fresh_process::announce_abort();
    Runtime rt(2);
    dftu_utils_test::ok(rt.block_on([]() -> coro::CoroTask<void> {
        coro::Scope scope;
        co_await scope.join();
        scope.spawn(noop());
    }()));
}

TEST_CASE("spawn into a closed scope" * doctest::skip()) {
    fresh_process::announce_abort();
    Runtime rt(2);
    dftu_utils_test::ok(rt.block_on([]() -> coro::CoroTask<void> {
        coro::Scope scope;
        scope.close();
        scope.spawn(noop());
        co_return;
    }()));
}

TEST_CASE("a child joins its own scope" * doctest::skip()) {
    fresh_process::announce_abort();
    Runtime rt(2);
    dftu_utils_test::ok(rt.block_on([]() -> coro::CoroTask<void> {
        coro::Scope scope;
        scope.spawn(
            [&scope]() -> coro::CoroTask<void> { co_await scope.join(); });
        co_await scope.join();
    }()));
}

TEST_CASE("spawn without an executor" * doctest::skip()) {
    fresh_process::announce_abort();
    coro::Scope scope(nullptr);
    scope.spawn(noop());
}

TEST_CASE("first_success without tasks" * doctest::skip()) {
    fresh_process::announce_abort();
    Runtime rt(2);
    dftu_utils_test::ok(
        rt.block_on(coro::first_success(dstd::Vector<coro::CoroTask<int>>{})));
}

TEST_CASE("parallel_for with a zero grain" * doctest::skip()) {
    fresh_process::announce_abort();
    Runtime rt(2);
    coro::ParallelForOptions options;
    options.grain = 0;
    dftu_utils_test::ok(rt.block_on(
        coro::parallel_for(10, [](std::int64_t, std::int64_t) {}, options)));
}

TEST_CASE("chunks of an async stream with size 0" * doctest::skip()) {
    fresh_process::announce_abort();
    Runtime rt(2);
    dftu_utils_test::ok(rt.block_on(drain(coro::chunks(numbers(3), 0))));
}

TEST_CASE("chunks of a generator with size 0" * doctest::skip()) {
    fresh_process::announce_abort();
    auto chunked = coro::chunks(sync_numbers(3), 0);
    for (auto it = chunked.begin(); it != chunked.end(); ++it) {
    }
}

TEST_CASE("buffer with size 0" * doctest::skip()) {
    fresh_process::announce_abort();
    Runtime rt(2);
    dftu_utils_test::ok(rt.block_on(drain(coro::buffer(numbers(3), 0))));
}

TEST_CASE("map_ordered with limit 0" * doctest::skip()) {
    fresh_process::announce_abort();
    Runtime rt(2);
    dftu_utils_test::ok(rt.block_on(drain(coro::map_ordered(
        numbers(3), [](int v) -> coro::CoroTask<int> { co_return v; }, 0))));
}

TEST_CASE("flat_map_merge with limit 0" * doctest::skip()) {
    fresh_process::announce_abort();
    Runtime rt(2);
    dftu_utils_test::ok(rt.block_on(drain(coro::flat_map_merge(
        numbers(3), [](int v) { return numbers(v); }, 0))));
}

TEST_CASE("limit with 0 runs" * doctest::skip()) {
    fresh_process::announce_abort();
    coro::limit(0);
}

TEST_CASE("ordered with 0 tokens" * doctest::skip()) {
    fresh_process::announce_abort();
    coro::ordered(0);
}

TEST_CASE("unordered with 0 tokens" * doctest::skip()) {
    fresh_process::announce_abort();
    coro::unordered(0);
}

TEST_CASE("a generator iterator past the end has no value" * doctest::skip()) {
    fresh_process::announce_abort();
    auto source = sync_numbers(0);
    auto it = source.begin();
    *it;
}

TEST_CASE("misuse of the coro API ends the process with its message") {
    struct Misuse {
        const char* test_case;
        const char* message;
    };
    const Misuse cases[] = {
        {"spawn after the join", "coro: a spawn into a closed scope"},
        {"spawn into a closed scope", "coro: a spawn into a closed scope"},
        {"a child joins its own scope",
         "coro: a task joined the scope that spawned it"},
        {"spawn without an executor",
         "coro: a spawn into a scope without a running executor"},
        {"first_success without tasks",
         "coro: first_success needs at least one task"},
        {"parallel_for with a zero grain",
         "coro: a parallel range needs a grain of at least 1"},
        {"chunks of an async stream with size 0",
         "coro: chunks needs a size above 0"},
        {"chunks of a generator with size 0",
         "coro: chunks needs a size above 0"},
        {"buffer with size 0", "coro: buffer needs a size above 0"},
        {"map_ordered with limit 0", "coro: map_ordered needs a limit above 0"},
        {"flat_map_merge with limit 0",
         "coro: flat_map_merge needs a limit above 0"},
        {"limit with 0 runs", "coro: limit needs at least one run"},
        {"ordered with 0 tokens", "coro: a stage needs at least 1 token"},
        {"unordered with 0 tokens", "coro: a stage needs at least 1 token"},
        {"a generator iterator past the end has no value",
         "coro: a generator iterator has no value"},
    };
    for (const Misuse& misuse : cases) {
        INFO(misuse.test_case);
        auto result = fresh_process::run(misuse.test_case);
        CHECK(result.aborted_with(misuse.message));
    }
}
