// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>

using namespace dftracer::utils;

namespace {

constexpr int OK = 0;
constexpr int RESTARTED = 10;
constexpr int NOT_STARTED_BEFORE = 11;
constexpr int REGISTERED_AFTER_STOP = 12;
constexpr int STARTED_AFTER_SHUTDOWN = 13;

/// Runs `body` in a forked child so the shutdown it causes does not reach
/// this process. Returns the child's exit code.
template <typename Body>
int in_child(Body body) {
    const pid_t pid = fork();
    if (pid == 0) _exit(body());
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

TEST_SUITE("ProcessTimers") {
    // Both cases fork before this process uses the timers, so the child owns
    // the first use.
    TEST_CASE("the timer thread stays stopped once shutdown stopped it") {
        const int code = in_child([] {
            auto& timers = coro::process_timers();
            if (!timers.running()) return NOT_STARTED_BEFORE;
            dstd::shutdown();
            auto& again = coro::process_timers();
            if (again.running()) return RESTARTED;
            const auto id =
                again.register_timeout(std::chrono::milliseconds(1), [] {});
            if (id != 0) return REGISTERED_AFTER_STOP;
            return OK;
        });
        CHECK(code == OK);
    }

    TEST_CASE("first use after shutdown starts nothing") {
        const int code = in_child([] {
            dstd::shutdown();
            auto& timers = coro::process_timers();
            if (timers.running()) return STARTED_AFTER_SHUTDOWN;
            const auto id =
                timers.register_timeout(std::chrono::milliseconds(1), [] {});
            if (id != 0) return REGISTERED_AFTER_STOP;
            return OK;
        });
        CHECK(code == OK);
    }
}
