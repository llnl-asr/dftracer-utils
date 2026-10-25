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

constexpr auto DEADLINE = std::chrono::seconds(10);

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
    constexpr int ROUNDS = 200000;
    int fired_rounds = 0;
    run_with_deadline("timer rounds", [&] {
        TimerService timers;
        timers.start();
        std::mutex m;
        std::condition_variable cv;
        for (int i = 0; i < ROUNDS; ++i) {
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
    CHECK(fired_rounds == ROUNDS);
}

TEST_CASE("stop wakes an idle timer thread") {
    run_with_deadline("start and stop", [] {
        for (int i = 0; i < 20000; ++i) {
            TimerService timers;
            timers.start();
            timers.stop();
        }
    });
}
