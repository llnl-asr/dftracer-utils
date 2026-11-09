// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
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
#include <stdexcept>

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

TEST_CASE("with_scope joins when its body throws") {
    Runtime rt(ExecutorConfig{.thread_count = 2}, false);
    std::atomic<int> done{0};
    CHECK_THROWS_AS(
        dftu_utils_test::ok(rt.block_on(coro::named(
            "throws",
            coro::with_scope([&](coro::Scope& s) -> coro::CoroTask<void> {
                s.spawn([&]() -> coro::CoroTask<void> {
                    ++done;
                    co_return;
                });
                throw std::runtime_error("body failed");
                co_return;
            })))),
        std::runtime_error);
    CHECK(done.load() == 1);
}
