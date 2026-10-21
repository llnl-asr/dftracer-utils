#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
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
        Runtime rt(ExecutorConfig{.num_threads = 2}, false);
        auto scope = CoroScope::long_lived(rt.executor());
        scope->spawn([](CoroScope&) -> coro::CoroTask<void> { co_return; });
        scope.reset();
    });
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);
}

TEST_CASE("a joined scope is destroyed without terminating") {
    int status = run_in_child([] {
        Runtime rt(ExecutorConfig{.num_threads = 2}, false);
        auto scope = CoroScope::long_lived(rt.executor());
        scope->spawn([](CoroScope&) -> coro::CoroTask<void> { co_return; });
        rt.run_blocking("join", [&](CoroScope&) -> coro::CoroTask<void> {
            co_await scope->join();
        });
        scope.reset();
    });
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("run_coro_scope joins when its body throws") {
    Runtime rt(ExecutorConfig{.num_threads = 2}, false);
    std::atomic<int> done{0};
    CHECK_THROWS_AS(
        rt.run_blocking("throws",
                        [&](CoroScope& s) -> coro::CoroTask<void> {
                            s.spawn([&](CoroScope&) -> coro::CoroTask<void> {
                                ++done;
                                co_return;
                            });
                            throw std::runtime_error("body failed");
                            co_return;
                        }),
        std::runtime_error);
    CHECK(done.load() == 1);
}
