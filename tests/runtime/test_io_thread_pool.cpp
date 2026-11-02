#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/io/io_thread_pool.h>
#include <dftracer/utils/runtime/scheduler/task_executor.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <dirent.h>
#endif

using namespace dftracer::utils;
using namespace std::chrono_literals;

namespace {

template <class Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

std::size_t process_threads() {
#if defined(__APPLE__)
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) return 0;
    for (mach_msg_type_number_t i = 0; i < count; ++i)
        mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list),
                  count * sizeof(thread_act_t));
    return count;
#elif defined(__linux__)
    std::size_t n = 0;
    if (DIR* d = ::opendir("/proc/self/task")) {
        while (const dirent* e = ::readdir(d))
            if (e->d_name[0] != '.') ++n;
        ::closedir(d);
    }
    return n;
#else
    return 0;
#endif
}

std::size_t settled_threads() {
    std::size_t last = process_threads();
    int stable = 0;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (stable < 5 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
        const std::size_t now = process_threads();
        stable = now == last ? stable + 1 : 0;
        last = now;
    }
    return last;
}

constexpr std::size_t CAP = 6;
constexpr std::chrono::milliseconds SHORT_KEEPALIVE{20};

io::IoPoolOptions elastic(std::chrono::milliseconds keepalive) {
    return io::IoPoolOptions{false, keepalive};
}

}  // namespace

TEST_CASE("an elastic pool starts with one thread") {
    io::IoThreadPool pool(CAP, 0, elastic(SHORT_KEEPALIVE));
    pool.start();
    CHECK(pool.live_threads() == 1);
    pool.stop();
    CHECK(pool.live_threads() == 0);
}

TEST_CASE("an eager pool starts the whole cap") {
    io::IoThreadPool pool(CAP, 0, io::IoPoolOptions{true, SHORT_KEEPALIVE});
    pool.start();
    CHECK(pool.live_threads() == CAP);
    std::this_thread::sleep_for(5 * SHORT_KEEPALIVE);
    CHECK(pool.live_threads() == CAP);
    pool.stop();
}

TEST_CASE("blocked requests grow the pool to the cap and no further") {
    io::IoThreadPool pool(CAP, 0, elastic(SHORT_KEEPALIVE));
    pool.start();
    std::atomic<bool> release{false};
    std::atomic<std::size_t> running{0};
    std::atomic<std::size_t> done{0};
    constexpr std::size_t JOBS = CAP * 3;
    for (std::size_t i = 0; i < JOBS; ++i) {
        pool.submit([&] {
            running.fetch_add(1);
            while (!release.load()) std::this_thread::sleep_for(1ms);
            done.fetch_add(1);
        });
    }
    REQUIRE(wait_until([&] { return running.load() == CAP; }));
    std::this_thread::sleep_for(30ms);
    CHECK(running.load() == CAP);
    CHECK(pool.live_threads() == CAP);
    release = true;
    REQUIRE(wait_until([&] { return done.load() == JOBS; }));
    pool.stop();
}

TEST_CASE("idle threads retire to the floor after the keepalive") {
    io::IoThreadPool pool(CAP, 0, elastic(SHORT_KEEPALIVE));
    pool.start();
    std::atomic<bool> release{false};
    std::atomic<std::size_t> running{0};
    for (std::size_t i = 0; i < CAP; ++i) {
        pool.submit([&] {
            running.fetch_add(1);
            while (!release.load()) std::this_thread::sleep_for(1ms);
        });
    }
    REQUIRE(wait_until([&] { return running.load() == CAP; }));
    release = true;
    CHECK(wait_until([&] { return pool.live_threads() == 1; }));
    std::this_thread::sleep_for(5 * SHORT_KEEPALIVE);
    CHECK(pool.live_threads() == 1);
    pool.stop();
}

TEST_CASE("a request submitted while threads retire still runs") {
    io::IoThreadPool pool(CAP, 0, elastic(1ms));
    pool.start();
    std::atomic<std::size_t> done{0};
    std::size_t submitted = 0;
    // Rounds until enough retirements raced a submit, since a loaded machine
    // retires fewer threads per round.
    for (std::size_t round = 0;
         round < 60 || (pool.retirements() < 20 && round < 6000); ++round) {
        for (std::size_t i = 0; i < CAP; ++i, ++submitted)
            pool.submit([&] {
                std::this_thread::sleep_for(500us);
                done.fetch_add(1);
            });
        std::this_thread::sleep_for(std::chrono::microseconds(round % 5 * 700));
        pool.submit([&] { done.fetch_add(1); });
        ++submitted;
    }
    REQUIRE(wait_until([&] { return done.load() == submitted; }));
    CHECK(pool.retirements() >= 20);
    pool.stop();
}

TEST_CASE("a burst after the pool shrank grows it again") {
    io::IoThreadPool pool(CAP, 0, elastic(1ms));
    pool.start();
    for (int round = 0; round < 20; ++round) {
        std::atomic<std::size_t> done{0};
        constexpr std::size_t JOBS = CAP * 2;
        for (std::size_t i = 0; i < JOBS; ++i)
            pool.submit([&] {
                std::this_thread::sleep_for(1ms);
                done.fetch_add(1);
            });
        REQUIRE(wait_until([&] { return done.load() == JOBS; }));
        std::this_thread::sleep_for(3ms);
    }
    pool.stop();
}

TEST_CASE("stop runs queued requests and joins every thread") {
    io::IoThreadPool pool(CAP, 0, elastic(SHORT_KEEPALIVE));
    pool.start();
    std::atomic<std::size_t> done{0};
    constexpr std::size_t JOBS = CAP * 4;
    for (std::size_t i = 0; i < JOBS; ++i)
        pool.submit([&] {
            std::this_thread::sleep_for(2ms);
            done.fetch_add(1);
        });
    pool.stop();
    CHECK(done.load() == JOBS);
    CHECK(pool.live_threads() == 0);
}

TEST_CASE("grow raises the cap without starting threads") {
    io::IoThreadPool pool(2, 0, elastic(SHORT_KEEPALIVE));
    pool.start();
    pool.grow(CAP, false);
    CHECK(pool.live_threads() == 1);
    pool.grow(CAP, true);
    CHECK(pool.live_threads() == CAP);
    pool.stop();
}

TEST_CASE("an eager executor holds more threads than an elastic one") {
    auto held = [](std::size_t min_workers) {
        const std::size_t before = settled_threads();
        ExecutorConfig config;
        config.num_threads = 2;
        config.io_pool_size = 8;
        config.min_workers = min_workers;
        config.io_backend_type = io::IoBackendType::Threadpool;
        auto executor = make_task_executor(config);
        executor->start();
        const std::size_t during = settled_threads();
        CHECK(executor->get_io_pool_size() == 8);
        executor->shutdown();
        return during - before;
    };
    if (process_threads() == 0) return;
    const std::size_t eager = held(0);
    const std::size_t elastic_held = held(1);
    CHECK(eager >= elastic_held + 6);
}
