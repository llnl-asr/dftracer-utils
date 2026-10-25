#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/executor/timer_service.h>
#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>

using namespace dftracer::utils;

namespace {

// Only a hang reaches this; a slow machine just runs fewer rounds.
constexpr auto DEADLINE = std::chrono::seconds(60);
constexpr auto RUN_FOR = std::chrono::seconds(3);

// A lost wakeup leaves the timer thread parked for good, so a failure would
// otherwise hang the test instead of reporting.
template <typename Fn>
void run_with_deadline(const char* what, Fn fn) {
    auto done = std::async(std::launch::async, std::move(fn));
    if (done.wait_for(DEADLINE) != std::future_status::ready) {
        std::fprintf(stderr, "%s did not finish within the deadline\n", what);
        std::_Exit(1);
    }
    done.get();
}

}  // namespace

TEST_CASE("each timer registered on an idle timer thread fires") {
    int rounds = 0;
    int fired_rounds = 0;
    run_with_deadline("timer rounds", [&] {
        TimerService timers;
        timers.start();
        std::mutex m;
        std::condition_variable cv;
        const auto until = std::chrono::steady_clock::now() + RUN_FOR;
        while (std::chrono::steady_clock::now() < until) {
            ++rounds;
            bool fired = false;
            timers.register_timeout(std::chrono::microseconds(0), [&] {
                std::lock_guard lock(m);
                fired = true;
                cv.notify_one();
            });
            std::unique_lock lock(m);
            if (!cv.wait_for(lock, std::chrono::seconds(2),
                             [&] { return fired; }))
                break;
            ++fired_rounds;
        }
        timers.stop();
    });
    MESSAGE("rounds: " << rounds);
    CHECK(rounds > 0);
    CHECK(fired_rounds == rounds);
}

TEST_CASE("stop wakes an idle timer thread") {
    run_with_deadline("start and stop", [] {
        const auto until = std::chrono::steady_clock::now() + RUN_FOR;
        while (std::chrono::steady_clock::now() < until) {
            TimerService timers;
            timers.start();
            timers.stop();
        }
    });
}
