// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/stage.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/dstd/either.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace std::chrono_literals;

namespace {

std::atomic<int> g_handler_calls{0};
std::atomic<int> g_active{0};
std::atomic<int> g_peak{0};

AsyncGenerator<int> numbers(int first, int count) {
    for (int i = 0; i < count; ++i) co_yield first + i;
}

template <typename T>
CoroTask<dstd::Vector<T>> collect(AsyncGenerator<T> source) {
    dstd::Vector<T> out;
    for (;;) {
        auto value = co_await source.next();
        if (!value) break;
        out.push_back(std::move(*value));
    }
    co_return out;
}

CoroTask<int> value_task(int v) { co_return v; }

CoroTask<int> failing_task() {
    co_await sleep(1ms);
    throw std::runtime_error("planned failure");
    co_return 0;
}

CoroTask<int> slow_value(std::chrono::milliseconds delay, int v) {
    co_await sleep(delay);
    co_return v;
}

CoroTask<int> gauge_run(std::chrono::milliseconds delay, int v) {
    const int now = ++g_active;
    int seen = g_peak.load();
    while (now > seen && !g_peak.compare_exchange_weak(seen, now)) {
    }
    co_await sleep(delay);
    --g_active;
    co_return v;
}

CoroTask<int> counting_handler(dstd::Error) {
    ++g_handler_calls;
    co_return -1;
}

AsyncGenerator<int> times_ten(AsyncGenerator<int> in) {
    for (;;) {
        auto v = co_await in.next();
        if (!v) break;
        co_yield *v * 10;
    }
}

AsyncGenerator<int> gate_stream(AsyncGenerator<int> in) {
    for (;;) {
        auto v = co_await in.next();
        if (!v) break;
        co_yield *v;
    }
}

struct Doubler {
    using Input = int;
    using Output = int;
    static constexpr const char* NAME = "doubler";
    CoroTask<int> run(int v) const { co_return v * 2; }
};

struct Tripler {
    using Input = int;
    using Output = int;
    static constexpr const char* NAME = "tripler";
    CoroTask<int> run(int v) const { co_return v * 3; }
};

struct ToText {
    using Input = int;
    using Output = dstd::String;
    static constexpr const char* NAME = "to_text";
    CoroTask<dstd::String> run(int v) const {
        co_return dstd::String(v == 0 ? "zero" : "many");
    }
};

struct Failing {
    using Input = int;
    using Output = int;
    static constexpr const char* NAME = "failing";
    CoroTask<int> run(int) const { return failing_task(); }
};

struct Gauge {
    using Input = int;
    using Output = int;
    static constexpr const char* NAME = "gauge";
    CoroTask<int> run(int v) const { return gauge_run(5ms, v); }
};

struct SlowStage {
    using Input = int;
    using Output = int;
    static constexpr const char* NAME = "slow";
    CoroTask<int> run(int v) const { return slow_value(30s, v); }
};

struct TimesTen {
    using Input = int;
    using Output = int;
    static constexpr const char* NAME = "times_ten";
    AsyncGenerator<int> run(AsyncGenerator<int> in) const {
        return times_ten(std::move(in));
    }
};

struct PassStream {
    using Input = int;
    using Output = int;
    static constexpr const char* NAME = "pass";
    AsyncGenerator<int> run(AsyncGenerator<int> in) const {
        return gate_stream(std::move(in));
    }
};

struct SumBox {
    using Input = dstd::UniquePtr<int>;
    using Output = int;
    static constexpr const char* NAME = "sum_box";
    CoroTask<int> run(dstd::UniquePtr<int> box) const { co_return *box + 1; }
};

CoroTask<dstd::UniquePtr<int>> boxed(int v) {
    co_return dstd::make_unique<int>(v);
}

template <typename S>
CoroTask<int> fan(S stage, int count) {
    dstd::Vector<CoroTask<int>> tasks;
    for (int i = 0; i < count; ++i) tasks.push_back(stage.run(i));
    auto results = co_await when_all(std::move(tasks));
    int sum = 0;
    for (int v : results) sum += v;
    co_return sum;
}

// The condition of the error a task raised, or of the error it gave as a
// value; nullopt when the task did not fail.
template <typename F>
dstd::Optional<dstd::Condition> condition_of(Runtime& runtime, F&& make_task) {
    using R = decltype(runtime.block_on(make_task()));
    try {
        if constexpr (std::is_void_v<R>) {
            runtime.block_on(make_task());
        } else {
            auto result = runtime.block_on(make_task());
            if constexpr (requires { result.error().condition(); }) {
                if (!result) return result.error().condition();
            }
        }
    } catch (const std::runtime_error&) {
        return dstd::Condition::Unknown;
    }
    return dstd::nullopt;
}

struct Counted {
    static inline int live = 0;
    Counted() { ++live; }
    Counted(Counted&&) noexcept { ++live; }
    Counted(const Counted&) { ++live; }
    ~Counted() { --live; }
};

struct Other {
    static inline int live = 0;
    Other() { ++live; }
    Other(Other&&) noexcept { ++live; }
    Other(const Other&) { ++live; }
    ~Other() { --live; }
};

CoroTask<int> left_task() { co_return 7; }
CoroTask<dstd::String> right_task() { co_return dstd::String("right"); }

CoroTask<bool> await_either(bool flag) {
    auto picked = co_await either(flag, left_task(), right_task());
    if (flag) co_return picked.is_left() && picked.left() == 7;
    co_return picked.is_right() && picked.right() == "right";
}

}  // namespace

TEST_CASE("x | adaptor is adaptor(x) and joined adaptors apply in order") {
    auto add_one = Adaptor{[](int x) { return x + 1; }};
    auto times_two = Adaptor{[](int x) { return x * 2; }};
    CHECK((5 | add_one) == add_one(5));
    auto joined = add_one | times_two;
    CHECK(joined(3) == 8);
    CHECK((3 | joined) == ((3 | add_one) | times_two));
    CHECK((3 | (times_two | add_one)) == 7);
}

TEST_CASE("then on a task takes a function, a task function and a stage") {
    Runtime runtime(2);
    CHECK(dftu_utils_test::ok(runtime.block_on(
              value_task(4) | then([](int v) { return v + 1; }))) == 5);
    CHECK(dftu_utils_test::ok(runtime.block_on(value_task(4) | then([](int v) {
                                                   return value_task(v * 3);
                                               }))) == 12);
    CHECK(dftu_utils_test::ok(
              runtime.block_on(value_task(4) | then(Doubler{}))) == 8);
    CHECK(dftu_utils_test::ok(
              runtime.block_on(then(value_task(4), Doubler{}))) == 8);
    auto text =
        dftu_utils_test::ok(runtime.block_on(value_task(0) | then(ToText{})));
    CHECK(text == "zero");
}

TEST_CASE("then on a generator runs per item or on the whole stream") {
    Runtime runtime(2);
    auto per_stage = dftu_utils_test::ok(
        runtime.block_on(collect(numbers(1, 3) | then(Doubler{}))));
    REQUIRE(per_stage.size() == 3);
    CHECK(per_stage[2] == 6);
    auto per_fn = dftu_utils_test::ok(runtime.block_on(
        collect(numbers(1, 3) | then([](int v) { return v + 100; }))));
    CHECK(per_fn[0] == 101);
    auto whole = dftu_utils_test::ok(
        runtime.block_on(collect(numbers(1, 3) | then(TimesTen{}))));
    REQUIRE(whole.size() == 3);
    CHECK(whole[1] == 20);
}

TEST_CASE("then on stages composes by kind and keeps the first name") {
    Runtime runtime(2);
    auto batch = Doubler{} | then(Tripler{});
    static_assert(BatchStage<decltype(batch)>);
    static_assert(std::is_same_v<decltype(batch)::Output, int>);
    CHECK(stage_name(batch) == "doubler");
    CHECK(dftu_utils_test::ok(runtime.block_on(batch.run(2))) == 12);

    auto texty = Doubler{} | then(ToText{});
    static_assert(std::is_same_v<decltype(texty)::Output, dstd::String>);
    CHECK(dftu_utils_test::ok(runtime.block_on(texty.run(0))) == "zero");

    auto streams = TimesTen{} | then(PassStream{});
    static_assert(StreamStage<decltype(streams)>);
    CHECK(stage_name(streams) == "times_ten");
    auto out = dftu_utils_test::ok(
        runtime.block_on(collect(streams.run(numbers(1, 3)))));
    REQUIRE(out.size() == 3);
    CHECK(out[2] == 30);

    auto mixed = TimesTen{} | then(Doubler{});
    static_assert(StreamStage<decltype(mixed)>);
    auto per_item = dftu_utils_test::ok(
        runtime.block_on(collect(mixed.run(numbers(1, 3)))));
    REQUIRE(per_item.size() == 3);
    CHECK(per_item[2] == 60);

    auto with_fn = Doubler{} | then([](int v) { return v + 1; });
    static_assert(BatchStage<decltype(with_fn)>);
    CHECK(dftu_utils_test::ok(runtime.block_on(with_fn.run(5))) == 11);
}

TEST_CASE("recover hands the error to the handler") {
    Runtime runtime(2);
    dstd::String seen;
    auto handler = [&seen](dstd::Error error) {
        seen = error.c_str();
        CHECK(error.condition() == dstd::Condition::Unknown);
        return -5;
    };
    CHECK(dftu_utils_test::ok(
              runtime.block_on(failing_task() | recover(handler))) == -5);
    CHECK(seen == "planned failure");
    CHECK(dftu_utils_test::ok(runtime.block_on(
              failing_task() | recover(counting_handler))) == -1);
    g_handler_calls = 0;
    CHECK(dftu_utils_test::ok(runtime.block_on(
              value_task(3) | recover(counting_handler))) == 3);
    CHECK(g_handler_calls.load() == 0);
    auto guarded = Failing{} | recover([](dstd::Error) { return 9; });
    static_assert(BatchStage<decltype(guarded)>);
    CHECK(dftu_utils_test::ok(runtime.block_on(guarded.run(1))) == 9);
}

TEST_CASE("recover does not swallow a cancel") {
    Runtime runtime(2);
    g_handler_calls = 0;
    const auto condition = condition_of(runtime, [] {
        return with_timeout(slow_value(30s, 1) | recover(counting_handler),
                            10ms);
    });
    CHECK(condition == dstd::Condition::Timeout);
    CHECK(g_handler_calls.load() == 0);

    bool cancelled = false;
    dftu_utils_test::ok(runtime.block_on([&]() -> CoroTask<void> {
        Scope scope;
        auto waiting = scope.spawn_future([]() -> CoroTask<int> {
            return slow_value(30s, 1) | recover(counting_handler);
        });
        co_await sleep(5ms);
        scope.request_stop();
        auto settled = co_await settle_in_place(waiting);
        cancelled = settled.stopped;
        co_await scope.join();
    }()));
    CHECK(cancelled);
    CHECK(g_handler_calls.load() == 0);
}

TEST_CASE("wrappers keep the kind and the outermost name wins") {
    static_assert(BatchStage<decltype(Doubler{} | named("a"))>);
    static_assert(BatchStage<decltype(Doubler{} | timeout(1s))>);
    static_assert(BatchStage<decltype(Doubler{} | limit(2))>);
    static_assert(StreamStage<decltype(TimesTen{} | named("a"))>);
    static_assert(StreamStage<decltype(TimesTen{} | limit(2))>);

    auto inner_first = Doubler{} | named("a") | timeout(1s);
    CHECK(stage_name(inner_first) == "a");
    auto outer_last = Doubler{} | timeout(1s) | named("z") | limit(1);
    CHECK(stage_name(outer_last) == "z");
    auto relabeled = Doubler{} | named("a") | named("b");
    CHECK(stage_name(relabeled) == "b");
    CHECK(stage_name(named("direct", Tripler{})) == "direct");
    CHECK(stage_name(Doubler{}) == "doubler");

    Runtime runtime(2);
    auto bounded = runtime.block_on(outer_last.run(4));
    REQUIRE(bounded);
    CHECK(*bounded == 8);
}

TEST_CASE("limit never exceeds n and frees its permit after an error") {
    Runtime runtime(4);
    g_active = 0;
    g_peak = 0;
    auto limited = Gauge{} | limit(2);
    auto copy = limited;
    dftu_utils_test::ok(runtime.block_on([&]() -> CoroTask<void> {
        auto a = fan(limited, 10);
        auto b = fan(copy, 10);
        auto both = co_await when_all(std::move(a), std::move(b));
        (void)both;
    }()));
    CHECK(g_peak.load() == 2);
    CHECK(g_active.load() == 0);

    auto single = Failing{} | limit(1);
    for (int round = 0; round < 3; ++round) {
        const auto condition = condition_of(
            runtime, [&] { return with_timeout(single.run(1), 2s); });
        CHECK(condition == dstd::Condition::Unknown);
    }
}

TEST_CASE("a stream frees its limit slot when it is dropped early") {
    Runtime runtime(2);
    auto limited = PassStream{} | limit(1);
    dftu_utils_test::ok(runtime.block_on([&]() -> CoroTask<void> {
        {
            auto stream = limited.run(numbers(0, 100));
            auto first = co_await stream.next();
            CHECK(first.has_value());
        }
        auto all =
            co_await with_timeout(collect(limited.run(numbers(0, 3))), 2s);
        REQUIRE(all);
        CHECK(all->size() == 3);
    }()));
}

TEST_CASE("timeout ends a slow batch stage and a slow task") {
    Runtime runtime(2);
    auto bounded = SlowStage{} | timeout(10ms);
    const auto start = std::chrono::steady_clock::now();
    CHECK(condition_of(runtime, [&] { return bounded.run(1); }) ==
          dstd::Condition::Timeout);
    CHECK(condition_of(runtime, [] {
              return slow_value(30s, 1) | timeout(10ms);
          }) == dstd::Condition::Timeout);
    // Far below the sleeps, so only a timeout that waits a sleep out fails,
    // while a slow shared runner still passes.
    CHECK(std::chrono::steady_clock::now() - start < 20s);
    auto chained = Doubler{} | timeout(5s) |
                   then([](dstd::Result<int> doubled) { return *doubled * 3; });
    CHECK(dftu_utils_test::ok(runtime.block_on(chained.run(1))) == 6);
}

TEST_CASE("maybe applies the adaptor or passes the value through") {
    auto add_one = Adaptor{[](int x) { return x + 1; }};
    CHECK((5 | maybe(true, add_one)) == 6);
    CHECK((5 | maybe(false, add_one)) == 5);
    Runtime runtime(2);
    CHECK(dftu_utils_test::ok(
              runtime.block_on(value_task(1) | maybe(true, limit(1)))) == 1);
    CHECK(dftu_utils_test::ok(
              runtime.block_on(value_task(1) | maybe(false, limit(1)))) == 1);
    auto out = dftu_utils_test::ok(runtime.block_on(collect(
        numbers(0, 3) | maybe(true, limit(1)) | maybe(false, limit(1)))));
    CHECK(out.size() == 3);
}

TEST_CASE("either holds only the chosen side") {
    {
        auto e = either(true, Counted{}, Other{});
        CHECK(e.is_left());
        CHECK(Counted::live == 1);
        CHECK(Other::live == 0);
    }
    {
        auto e = either(false, Counted{}, Other{});
        CHECK(e.is_right());
        CHECK(Counted::live == 0);
        CHECK(Other::live == 1);
    }
    CHECK(Counted::live == 0);
    CHECK(Other::live == 0);
}

TEST_CASE("co_await on an Either of tasks gives an Either of values") {
    Runtime runtime(2);
    CHECK(dftu_utils_test::ok(runtime.block_on(await_either(true))));
    CHECK(dftu_utils_test::ok(runtime.block_on(await_either(false))));
}

TEST_CASE("an Either of stages works where a stage is taken") {
    Runtime runtime(2);
    auto same = either(true, Doubler{}, Tripler{});
    auto composed = same | then([](int v) { return v + 1; });
    static_assert(BatchStage<decltype(composed)>);
    CHECK(dftu_utils_test::ok(runtime.block_on(composed.run(5))) == 11);
    auto other = either(false, Doubler{}, Tripler{}) | named("pick");
    CHECK(stage_name(other) == "pick");
    CHECK(dftu_utils_test::ok(runtime.block_on(other.run(5))) == 15);

    auto mixed = as_stage(either(false, Doubler{}, ToText{}));
    static_assert(std::is_same_v<decltype(mixed)::Output,
                                 dstd::Either<int, dstd::String>>);
    auto picked = dftu_utils_test::ok(runtime.block_on(mixed.run(0)));
    REQUIRE(picked.is_right());
    CHECK(picked.right() == "zero");
    auto left = dftu_utils_test::ok(
        runtime.block_on(as_stage(either(true, Doubler{}, ToText{})).run(4)));
    REQUIRE(left.is_left());
    CHECK(left.left() == 8);
    CHECK(stage_name(mixed) == "to_text");

    auto limited = either(true, Doubler{}, Tripler{}) | limit(1) | timeout(5s);
    auto bounded = runtime.block_on(limited.run(2));
    REQUIRE(bounded);
    CHECK(*bounded == 4);
    auto erased = erase(either(false, Doubler{}, Tripler{}));
    CHECK(dftu_utils_test::ok(runtime.block_on(erased.run(2))) == 6);
}

TEST_CASE("erase moves, runs and forwards the name") {
    Runtime runtime(2);
    auto erased = erase(Doubler{});
    static_assert(BatchStage<decltype(erased)>);
    static_assert(!std::is_copy_constructible_v<decltype(erased)>);
    static_assert(std::is_nothrow_move_constructible_v<decltype(erased)>);
    CHECK(stage_name(erased) == "doubler");
    auto moved = std::move(erased);
    CHECK(dftu_utils_test::ok(runtime.block_on(moved.run(21))) == 42);

    auto wrapped = erase(Doubler{} | named("custom"));
    CHECK(stage_name(wrapped) == "custom");

    auto stream = erase(TimesTen{});
    static_assert(StreamStage<decltype(stream)>);
    CHECK(stage_name(stream) == "times_ten");
    auto stream_moved = std::move(stream);
    auto out = dftu_utils_test::ok(
        runtime.block_on(collect(stream_moved.run(numbers(1, 3)))));
    REQUIRE(out.size() == 3);
    CHECK(out[2] == 30);

    auto chained = erase(Doubler{}) | then(Tripler{});
    CHECK(dftu_utils_test::ok(runtime.block_on(chained.run(1))) == 6);
}

TEST_CASE("a move-only value passes through then and erase") {
    Runtime runtime(2);
    CHECK(dftu_utils_test::ok(runtime.block_on(boxed(4) | then(SumBox{}))) ==
          5);
    auto erased = erase(SumBox{});
    CHECK(dftu_utils_test::ok(
              runtime.block_on(erased.run(dstd::make_unique<int>(9)))) == 10);
    auto composed = SumBox{} | then(Doubler{});
    CHECK(dftu_utils_test::ok(
              runtime.block_on(composed.run(dstd::make_unique<int>(1)))) == 4);
}

TEST_CASE("timeout bounds each item of a stream") {
    Runtime runtime(2);
    auto slow_stream = []() -> AsyncGenerator<int> {
        co_yield 1;
        co_await sleep(60s);
        co_yield 2;
    };
    dstd::Condition condition = dstd::Condition::Unknown;
    int seen = 0;
    const auto start = std::chrono::steady_clock::now();
    dftu_utils_test::ok(runtime.block_on([&]() -> CoroTask<void> {
        auto stream = slow_stream() | timeout(20ms);
        for (;;) {
            auto item = co_await stream.next();
            if (!item) break;
            if (!*item) {
                condition = item->error().condition();
                break;
            }
            ++seen;
        }
    }()));
    CHECK(seen == 1);
    CHECK(condition == dstd::Condition::Timeout);
    // Far below the sleep, so only a timeout that waits the sleep out fails,
    // while a slow shared runner still passes.
    CHECK(std::chrono::steady_clock::now() - start < 20s);

    auto fast = dftu_utils_test::ok(
        runtime.block_on(collect(numbers(0, 100) | timeout(5s))));
    CHECK(fast.size() == 100);
}

TEST_CASE("timeout keeps a stream stage a stream stage") {
    Runtime runtime(2);
    auto bounded = PassStream{} | timeout(5s);
    static_assert(StreamStage<decltype(bounded)>);
    auto out = dftu_utils_test::ok(
        runtime.block_on(collect(bounded.run(numbers(0, 6)))));
    CHECK(out.size() == 6);
}

TEST_CASE("then takes a move-only stage and its adaptor applies many times") {
    Runtime runtime(2);
    auto triple = then(erase(Tripler{}));
    CHECK(dftu_utils_test::ok(runtime.block_on(value_task(2) | triple)) == 6);
    auto composed = Doubler{} | triple;
    static_assert(BatchStage<decltype(composed)>);
    CHECK(dftu_utils_test::ok(runtime.block_on(composed.run(1))) == 6);
    auto per_item =
        dftu_utils_test::ok(runtime.block_on(collect(numbers(1, 2) | triple)));
    REQUIRE(per_item.size() == 2);
    CHECK(per_item[1] == 6);

    auto stream_next = PassStream{} | then(erase(PassStream{}));
    static_assert(StreamStage<decltype(stream_next)>);
    auto passed = dftu_utils_test::ok(
        runtime.block_on(collect(stream_next.run(numbers(0, 3)))));
    CHECK(passed.size() == 3);

    auto two_arg = then(Doubler{}, erase(Tripler{}));
    CHECK(dftu_utils_test::ok(runtime.block_on(two_arg.run(1))) == 6);
}
