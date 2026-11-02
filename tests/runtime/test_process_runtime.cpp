// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

using dftracer::utils::configure_default_runtime;
using dftracer::utils::default_runtime;
using dftracer::utils::default_runtime_shared;
using dftracer::utils::Runtime;
using dftracer::utils::TaskHandle;
using dftracer::utils::coro::CoroTask;

namespace {

struct Concurrency {
    std::atomic<int> now{0};
    std::atomic<int> most{0};
};

CoroTask<void> busy(Concurrency* c, std::chrono::milliseconds hold) {
    const int running = c->now.fetch_add(1) + 1;
    int seen = c->most.load();
    while (running > seen && !c->most.compare_exchange_weak(seen, running)) {
    }
    std::this_thread::sleep_for(hold);
    c->now.fetch_sub(1);
    co_return;
}

int most_at_once(int tasks) {
    Concurrency c;
    std::vector<TaskHandle> handles;
    for (int i = 0; i < tasks; ++i)
        handles.push_back(default_runtime().submit(
            busy(&c, std::chrono::milliseconds(50)), "busy"));
    for (auto& h : handles) h.wait();
    return c.most.load();
}

CoroTask<void> tiny() { co_return; }

std::chrono::microseconds cpu_time() {
    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
    const auto us = [](const timeval& t) {
        return std::chrono::seconds(t.tv_sec) +
               std::chrono::microseconds(t.tv_usec);
    };
    return std::chrono::duration_cast<std::chrono::microseconds>(
        us(usage.ru_utime) + us(usage.ru_stime));
}

CoroTask<void> wait_for(std::atomic<bool>* go, std::atomic<bool>* done) {
    while (!go->load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    done->store(true);
    co_return;
}

}  // namespace

TEST_SUITE("ProcessRuntime") {
    TEST_CASE("threads that ask at once all get one runtime") {
        std::vector<std::shared_ptr<Runtime>> got(16);
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < got.size(); ++i)
            threads.emplace_back([&, i] { got[i] = default_runtime_shared(); });
        for (auto& t : threads) t.join();
        for (const auto& rt : got) CHECK(rt.get() == got.front().get());
    }

    TEST_CASE("configure from many threads at once keeps one runtime") {
        Runtime* before = &default_runtime();
        std::vector<Runtime*> got(8);
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < got.size(); ++i)
            threads.emplace_back([&, i] {
                got[i] = &configure_default_runtime({.threads = i + 1});
            });
        for (auto& t : threads) t.join();
        for (Runtime* rt : got) CHECK(rt == before);
        CHECK(default_runtime().threads() >= 1);
        CHECK(default_runtime().threads() <= got.size());
    }

    TEST_CASE("work started before a configure runs on the same runtime") {
        configure_default_runtime({.threads = 2});
        Runtime* before = &default_runtime();
        std::atomic<bool> go{false};
        std::atomic<bool> done{false};
        auto handle = default_runtime().submit(wait_for(&go, &done), "early");
        Runtime& after = configure_default_runtime({.threads = 3});
        go = true;
        handle.wait();
        CHECK(done.load());
        CHECK(&after == before);
        CHECK(after.threads() == 3);
    }

    TEST_CASE("a configure moves the running cap up and down") {
        configure_default_runtime({.threads = 1, .eager = true});
        CHECK(most_at_once(4) == 1);
        configure_default_runtime({.threads = 4});
        CHECK(most_at_once(4) > 1);
        configure_default_runtime({.threads = 1});
        CHECK(most_at_once(4) == 1);
        configure_default_runtime({.threads = 2, .eager = false});
        CHECK(most_at_once(4) <= 2);
    }

    TEST_CASE(
        "workers wait, not spin, after a shrink below the running tasks") {
        configure_default_runtime({.threads = 8, .eager = true});
        Concurrency held;
        std::vector<TaskHandle> long_tasks;
        for (int i = 0; i < 3; ++i)
            long_tasks.push_back(default_runtime().submit(
                busy(&held, std::chrono::milliseconds(600)), "long"));
        while (held.now.load() < 3)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // Three tasks hold permits of a cap of two: no permit is free until
        // two of them end.
        configure_default_runtime({.threads = 2});
        std::vector<TaskHandle> waiting;
        for (int i = 0; i < 4; ++i)
            waiting.push_back(default_runtime().submit(tiny(), "tiny"));
        const auto before = cpu_time();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto used = cpu_time() - before;
        for (auto& h : long_tasks) h.wait();
        for (auto& h : waiting) h.wait();
        MESSAGE("cpu while waiting: " << used.count() << " us");
        CHECK(used < std::chrono::milliseconds(150));
    }

    TEST_CASE("an elastic pool of one stays elastic when it grows") {
        configure_default_runtime({.threads = 1, .eager = false});
        // Let the pool retire down to its floor of one.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (default_runtime().get_progress().workers.size() > 1 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        REQUIRE(default_runtime().get_progress().workers.size() == 1);
        configure_default_runtime({.threads = 8});
        CHECK(default_runtime().get_progress().workers.size() < 8);
    }

    TEST_CASE("shutdown of the process runtime leaves it running") {
        default_runtime().shutdown();
        CHECK(most_at_once(1) == 1);
    }

    TEST_CASE("the child of a fork makes its own runtime") {
#ifdef DFTRACER_UTILS_TSAN_ENABLED
        // ThreadSanitizer stops a child of a multi-threaded fork that starts
        // a thread, which this child must.
        MESSAGE("not run under ThreadSanitizer");
        return;
#endif
        CHECK(most_at_once(1) == 1);
        const pid_t child = ::fork();
        REQUIRE(child >= 0);
        if (child == 0) {
            const bool ok = most_at_once(2) >= 1;
            ::_exit(ok ? 0 : 1);
        }
        int status = 0;
        REQUIRE(::waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
    }
}
