#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <doctest/doctest.h>

#include <atomic>

using namespace dftracer::utils;

TEST_CASE("CoroScope - Simple spawn test") {
    Executor executor(2);
    Scheduler scheduler(&executor);

    std::atomic<int> counter{0};

    auto task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            fprintf(stderr, "Main task: before ctx.scope()\n");
            co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
                fprintf(stderr, "Scope lambda: spawning tasks\n");
                // Spawn 3 simple tasks
                for (int i = 0; i < 3; ++i) {
                    scope.spawn([&](CoroScope&) -> coro::CoroTask<void> {
                        fprintf(stderr, "Spawned task executing\n");
                        ++counter;
                        co_return;
                    });
                }
                fprintf(stderr, "Scope lambda: returning\n");
                co_return;
            });
            fprintf(stderr, "Main task: after ctx.scope(), counter=%d\n",
                    counter.load());
            co_return;
        },
        "SimpleTest");

    scheduler.schedule(task);
    fprintf(stderr, "Test: waiting for task to complete\n");
    task->wait();
    fprintf(stderr, "Test: task completed\n");

    CHECK(counter.load() == 3);
    fprintf(stderr, "Test: assertion passed, counter=%d\n", counter.load());

    executor.shutdown();
    fprintf(stderr, "Test: executor shutdown complete\n");
}
