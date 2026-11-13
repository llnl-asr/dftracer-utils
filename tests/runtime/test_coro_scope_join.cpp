// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <coro/failing.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <no_core_dump.h>
#include <sys/wait.h>
#include <task_result.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>

using namespace dftracer::utils;

namespace {

template <typename Body>
int run_in_child(Body body) {
    pid_t pid = ::fork();
    if (pid == 0) {
        no_core_dump::disable();
        body();
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return status;
}

}  // namespace

TEST_CASE("an un-joined scope terminates") {
    int status = run_in_child([] {
        Runtime rt(ExecutorConfig{.thread_count = 2}, false);
        auto scope = dstd::make_unique<coro::Scope>(rt.executor());
        scope->spawn([]() -> coro::CoroTask<void> { co_return; });
        scope.reset();
    });
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);
}

TEST_CASE("a joined scope is destroyed without terminating") {
    int status = run_in_child([] {
        Runtime rt(ExecutorConfig{.thread_count = 2}, false);
        auto scope = dstd::make_unique<coro::Scope>(rt.executor());
        scope->spawn([]() -> coro::CoroTask<void> { co_return; });
        dftu_utils_test::ok(
            rt.block_on(coro::named("join", [&]() -> coro::CoroTask<void> {
                co_await scope->join();
            }())));
        scope.reset();
    });
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("with_scope joins when its body fails") {
    Runtime rt(ExecutorConfig{.thread_count = 2}, false);
    std::atomic<int> done{0};
    const auto result = rt.block_on(coro::named(
        "fails", coro::with_scope([&](coro::Scope& s) -> coro::CoroTask<void> {
            s.spawn([&]() -> coro::CoroTask<void> {
                ++done;
                co_return;
            });
            co_await coro_test::failing_task("body failed");
        })));
    REQUIRE_FALSE(result.has_value());
    CHECK(dstd::String(result.error().message()) == "body failed");
    CHECK(done.load() == 1);
}
