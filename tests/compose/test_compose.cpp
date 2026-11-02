// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/runtime/compose.h>
#include <dftracer/utils/runtime/pipeline/pipeline.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/task_graph/task_graph.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace dftracer::utils;

using dftracer::utils::CoroScope;
using dftracer::utils::make_task;
using dftracer::utils::Pipeline;
using dftracer::utils::PipelineConfig;
using dftracer::utils::Runtime;
namespace tg = dftracer::utils::task_graph;
namespace u = dftracer::utils::utilities;

namespace {

// Plain async ops: callables of shape I -> coro::CoroTask<O>. One is a struct,
// one is a lambda, to show either satisfies the contract.
struct Doubler {
    coro::CoroTask<int> operator()(int x) const { co_return x * 2; }
};

// A context-taking op offering both contracts: inject a scope (DI), or call
// scope-less and let it provide one via with_scope. It fans two spawns out.
struct ScopedSum {
    coro::CoroTask<int> operator()(coro::Scope& scope, int x) const {
        auto a =
            scope.spawn_future([x]() -> coro::CoroTask<int> { co_return x; });
        auto b = scope.spawn_future(
            [x]() -> coro::CoroTask<int> { co_return x * 10; });
        co_return (co_await a) + (co_await b);
    }
    coro::CoroTask<int> operator()(int x) const {
        return coro::with_scope(
            [this, x](coro::Scope& scope) { return (*this)(scope, x); });
    }
};

auto stringify = [](int x) -> coro::CoroTask<std::string> {
    co_return std::to_string(x);
};

}  // namespace

TEST_SUITE("compose") {
    TEST_CASE("pipe (|) chains ops, run on the runtime") {
        Runtime rt;
        auto pipeline = u::op(Doubler{}) | u::op(stringify);
        std::string out;
        rt.block_on(coro::named("pipe", [&]() -> coro::CoroTask<void> {
            out = co_await pipeline(21);
        }()));
        CHECK(out == "42");
    }

    TEST_CASE("all (&&) fans out, yields a tuple") {
        Runtime rt;
        auto fan = u::op(Doubler{}) && u::op(stringify);
        std::tuple<int, std::string> out;
        rt.block_on(coro::named("all", [&]() -> coro::CoroTask<void> {
            out = co_await fan(21);
        }()));
        CHECK(std::get<0>(out) == 42);
        CHECK(std::get<1>(out) == "21");
    }

    TEST_CASE("all (&&) runs both ops at the same time") {
        Runtime rt(2);
        std::atomic<bool> second_started{false};
        auto first = [&second_started](int) -> coro::CoroTask<bool> {
            for (int i = 0; i < 100000 && !second_started.load(); ++i) {
                co_await dftracer::utils::coro::yield();
            }
            co_return second_started.load();
        };
        auto second = [&second_started](int x) -> coro::CoroTask<int> {
            second_started.store(true);
            co_return x;
        };
        auto both = u::op(first) && u::op(second);
        std::tuple<bool, int> out;
        rt.block_on(coro::named("all", [&]() -> coro::CoroTask<void> {
            out = co_await both(7);
        }()));
        CHECK(std::get<0>(out));
        CHECK(std::get<1>(out) == 7);
    }

    TEST_CASE("any (||) races, yields the first to finish") {
        Runtime rt;
        auto fast = [](int x) -> coro::CoroTask<int> { co_return x + 1; };
        auto slow = [](int x) -> coro::CoroTask<int> { co_return x + 1000; };
        auto race = u::op(fast) || u::op(slow);
        int out = -1;
        rt.block_on(coro::named("any", [&]() -> coro::CoroTask<void> {
            out = co_await race(41);
        }()));
        CHECK((out == 42 || out == 1041));
    }

    TEST_CASE("context op: inject a scope (DI) vs self-scoped overload") {
        Runtime rt;
        ScopedSum sum;
        // Contract 1: caller injects its scope.
        int injected = -1;
        rt.block_on(coro::named(
            "inject",
            coro::with_scope([&](coro::Scope& s) -> coro::CoroTask<void> {
                injected = co_await sum(s, 3);
            })));
        CHECK(injected == 33);  // 3 + 30
        // Contract 2: scope-less call, op provides its own scope.
        int standalone = rt.submit(sum(4)).get();
        CHECK(standalone == 44);  // 4 + 40
    }

    TEST_CASE("static submit: zero erasure, op is just a coroutine") {
        Runtime rt;
        Doubler d;
        int out = rt.submit(d(21)).get();
        CHECK(out == 42);
    }

    TEST_CASE("map lifts an op over a vector, concurrently") {
        Runtime rt;
        auto doubled = u::map(Doubler{});
        std::vector<int> out;
        rt.block_on(coro::named("map", [&]() -> coro::CoroTask<void> {
            // GCC 12.1 rejects a braced list inside co_await.
            std::vector<int> in{1, 2, 3, 4};
            out = co_await doubled(std::move(in));
        }()));
        CHECK(out == std::vector<int>{2, 4, 6, 8});
    }

    TEST_CASE("map | fold is parallel map-reduce (what BatchProcessor did)") {
        Runtime rt;
        auto sum_of_doubles =
            u::map(Doubler{}) |
            u::fold(0, [](int acc, int x) { return acc + x; });
        int out = -1;
        rt.block_on(coro::named("mapreduce", [&]() -> coro::CoroTask<void> {
            std::vector<int> in{1, 2, 3, 4};
            out = co_await sum_of_doubles(std::move(in));
        }()));
        CHECK(out == 20);  // (2+4+6+8)
    }

    TEST_CASE("a compose op drops straight into the real task_graph DAG") {
        // A plain compose pipeline used as a graph node, run through the
        // actual scheduler.
        auto pipe = u::op(Doubler{}) | u::op([](int x) -> coro::CoroTask<int> {
                        co_return x + 1;
                    });

        auto source = make_task(
            [](CoroScope&) -> coro::CoroTask<int> { co_return 20; }, "src");
        auto graph = tg::TaskGraph::builder({.name = "compose_in_dag"});
        auto wrapped = graph.wrap<int>(source);
        auto mapped =
            graph.map<int>(wrapped,
                           [pipe](CoroScope&, int v) -> coro::CoroTask<int> {
                               co_return co_await pipe(v);
                           },
                           {.name = "compose"});

        Pipeline pipeline(PipelineConfig::parallel(2));
        pipeline.set_source(source);
        pipeline.execute();
        CHECK(mapped.task()->get<int>() == 41);  // (20 * 2) + 1
    }
}
