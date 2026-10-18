#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/compose.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/pipeline/pipeline.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/task_graph/task_graph.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/runtime/tasks/task.h>
#include <doctest/doctest.h>

#include <string>
#include <tuple>

using dftracer::utils::CoroScope;
using dftracer::utils::make_task;
using dftracer::utils::Pipeline;
using dftracer::utils::PipelineConfig;
using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
namespace tg = dftracer::utils::task_graph;
namespace u = dftracer::utils::utilities;

namespace {

// Plain async ops: callables of shape I -> CoroTask<O>. One is a struct, one
// is a lambda, to show either satisfies the contract.
struct Doubler {
    CoroTask<int> operator()(int x) const { co_return x * 2; }
};

// A context-taking op offering both contracts: inject a scope (DI), or call
// scope-less and let it provide one via with_scope. It fans two spawns out.
struct ScopedSum {
    CoroTask<int> operator()(CoroScope& scope, int x) const {
        auto a = scope.spawn([x](CoroScope&) -> CoroTask<int> { co_return x; });
        auto b =
            scope.spawn([x](CoroScope&) -> CoroTask<int> { co_return x * 10; });
        co_return (co_await a) + (co_await b);
    }
    CoroTask<int> operator()(int x) const {
        return dftracer::utils::with_scope(*this, x);
    }
};

auto stringify = [](int x) -> CoroTask<std::string> {
    co_return std::to_string(x);
};

}  // namespace

TEST_SUITE("compose") {
    TEST_CASE("pipe (|) chains ops, run on the runtime") {
        Runtime rt;
        auto pipeline = u::op(Doubler{}) | u::op(stringify);
        std::string out;
        rt.run_blocking("pipe", [&](auto&) -> CoroTask<void> {
            out = co_await pipeline(21);
        });
        CHECK(out == "42");
    }

    TEST_CASE("all (&&) fans out, yields a tuple") {
        Runtime rt;
        auto fan = u::op(Doubler{}) && u::op(stringify);
        std::tuple<int, std::string> out;
        rt.run_blocking(
            "all", [&](auto&) -> CoroTask<void> { out = co_await fan(21); });
        CHECK(std::get<0>(out) == 42);
        CHECK(std::get<1>(out) == "21");
    }

    TEST_CASE("any (||) races, yields the first to finish") {
        Runtime rt;
        auto fast = [](int x) -> CoroTask<int> { co_return x + 1; };
        auto slow = [](int x) -> CoroTask<int> { co_return x + 1000; };
        auto race = u::op(fast) || u::op(slow);
        int out = -1;
        rt.run_blocking(
            "any", [&](auto&) -> CoroTask<void> { out = co_await race(41); });
        CHECK((out == 42 || out == 1041));
    }

    TEST_CASE("context op: inject a scope (DI) vs self-scoped overload") {
        Runtime rt;
        ScopedSum sum;
        // Contract 1: caller injects its scope.
        int injected = -1;
        rt.run_blocking("inject", [&](CoroScope& s) -> CoroTask<void> {
            injected = co_await sum(s, 3);
        });
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
        rt.run_blocking("map", [&](auto&) -> CoroTask<void> {
            out = co_await doubled(std::vector<int>{1, 2, 3, 4});
        });
        CHECK(out == std::vector<int>{2, 4, 6, 8});
    }

    TEST_CASE("map | fold is parallel map-reduce (what BatchProcessor did)") {
        Runtime rt;
        auto sum_of_doubles =
            u::map(Doubler{}) |
            u::fold(0, [](int acc, int x) { return acc + x; });
        int out = -1;
        rt.run_blocking("mapreduce", [&](auto&) -> CoroTask<void> {
            out = co_await sum_of_doubles(std::vector<int>{1, 2, 3, 4});
        });
        CHECK(out == 20);  // (2+4+6+8)
    }

    TEST_CASE("a compose op drops straight into the real task_graph DAG") {
        // A plain compose pipeline used as a graph node, run through the
        // actual scheduler.
        auto pipe = u::op(Doubler{}) |
                    u::op([](int x) -> CoroTask<int> { co_return x + 1; });

        auto source =
            make_task([](CoroScope&) -> CoroTask<int> { co_return 20; }, "src");
        auto graph = tg::TaskGraph::builder({.name = "compose_in_dag"});
        auto wrapped = graph.wrap<int>(source);
        auto mapped =
            graph.map<int>(wrapped,
                           [pipe](CoroScope&, int v) -> CoroTask<int> {
                               co_return co_await pipe(v);
                           },
                           {.name = "compose"});

        Pipeline pipeline(PipelineConfig::parallel(2));
        pipeline.set_source(source);
        pipeline.execute();
        CHECK(mapped.task()->get<int>() == 41);  // (20 * 2) + 1
    }
}
