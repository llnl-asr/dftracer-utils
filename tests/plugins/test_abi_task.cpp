// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <coro/failing.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/plugins/host/port_hub.h>
#include <dftracer/utils/plugins/task_abi.h>
#include <doctest/doctest.h>
#include <plugins/test_abi_ctx.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace dftracer::utils;
using test_plugin_common::run_task;

namespace {

coro::CoroTask<void> set_flag(std::atomic<int>* n) {
    n->fetch_add(1);
    co_return;
}

coro::CoroTask<void> fail_boom() { co_await coro_test::failing_task("boom"); }

const dftu_svc_task* task_service() {
    return static_cast<const dftu_svc_task*>(
        plugins::host::detail::task_ext_vtable());
}

// A spawned function that runs until it is cancelled.
struct Spin {
    std::atomic<bool> started{false};
    std::atomic<int> runs{0};
};

dftu_status spin_until_cancelled(dftu_ctx* ctx, void* arg) {
    auto* spin = static_cast<Spin*>(arg);
    spin->runs.fetch_add(1);
    spin->started.store(true);
    while (dftu_check(ctx) == DFTU_OK) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return dftu_check(ctx);
}

coro::CoroTask<void> run_inner(plugins::AbiTask* inner, std::atomic<int>* ended,
                               dftu_status* status) {
    *status = co_await plugins::run_abi_task(inner);
    ended->fetch_add(1);
}

coro::AsyncGenerator<int> parked_generator(coro::AsyncSemaphore* sem) {
    co_await sem->acquire(1);
    co_yield 1;
}

coro::CoroTask<void> read_one(coro::AsyncSemaphore* sem,
                              std::atomic<int>* yielded) {
    auto gen = parked_generator(sem);
    auto next = co_await gen.next();
    if (next) yielded->fetch_add(1);
}

bool wait_for(const std::atomic<bool>& flag) {
    for (int i = 0; i < 500 && !flag.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return flag.load();
}

}  // namespace

TEST_SUITE("plugin task ABI") {
    TEST_CASE("the task service run drives a task to completion") {
        std::atomic<int> ran{0};
        dftu_task* t = plugins::task_to_abi(set_flag(&ran));
        CHECK(run_task(t) == DFTU_OK);
        CHECK(ran.load() == 1);
    }

    TEST_CASE("the task service run reports a failed task with its message") {
        dftu_task* t = plugins::task_to_abi(fail_boom());
        dstd::String message;
        CHECK(run_task(t, &message) == DFTU_E_UNKNOWN);
        CHECK(message.find("boom") != dstd::String::npos);
    }

    TEST_CASE("a task made with make_abi_task fails with its own status") {
        dftu_task* t = plugins::make_abi_task([](plugins::AbiTask* task) {
            return [](plugins::AbiTask* self) -> coro::CoroTask<void> {
                self->fail(DFTU_E_NOT_FOUND, "no such thing");
                co_return;
            }(task);
        });
        dstd::String message;
        CHECK(run_task(t, &message) == DFTU_E_NOT_FOUND);
        CHECK(message == "no such thing");
    }

    TEST_CASE("run_abi_task awaits a task once and repeats its status") {
        std::atomic<int> ran{0};
        dftu_task* t = plugins::task_to_abi(set_flag(&ran));
        plugins::AbiTask* task = plugins::abi_task(t);
        dftu_status first = DFTU_E_UNKNOWN, second = DFTU_E_UNKNOWN;
        auto body = [&]() -> coro::CoroTask<void> {
            first = co_await plugins::run_abi_task(task);
            second = co_await plugins::run_abi_task(task);
        };
        dftu_task* driver = plugins::task_to_abi(body());
        CHECK(run_task(driver) == DFTU_OK);
        CHECK(first == DFTU_OK);
        CHECK(second == DFTU_OK);
        CHECK(ran.load() == 1);
        plugins::free_abi_task(t);
    }

    TEST_CASE("test tells a finished task from one that has not run") {
        std::atomic<int> ran{0};
        dftu_task* t = plugins::task_to_abi(set_flag(&ran));
        const dftu_svc_task* svc = task_service();
        CHECK(svc->test(nullptr, t) == 0);
        CHECK(svc->test(nullptr, nullptr) == DFTU_E_INVALID);
        plugins::AbiTask* task = plugins::abi_task(t);
        dftu_status status = DFTU_E_UNKNOWN;
        auto body = [&]() -> coro::CoroTask<void> {
            status = co_await plugins::run_abi_task(task);
        };
        CHECK(run_task(plugins::task_to_abi(body())) == DFTU_OK);
        CHECK(status == DFTU_OK);
        CHECK(svc->test(nullptr, t) == 1);
        // A task that finished ignores a cancel and keeps its status.
        svc->cancel(nullptr, t);
        dftu_status again = DFTU_E_UNKNOWN;
        auto repeat = [&]() -> coro::CoroTask<void> {
            again = co_await plugins::run_abi_task(task);
        };
        CHECK(run_task(plugins::task_to_abi(repeat())) == DFTU_OK);
        CHECK(again == DFTU_OK);
        plugins::free_abi_task(t);
    }

    TEST_CASE("cancel stops a running task, which ends once as cancelled") {
        Spin spin;
        const dftu_svc_task* svc = task_service();
        test_plugin_common::Ctx ctx;
        dftu_task* t = nullptr;
        REQUIRE(svc->spawn(ctx.get(), nullptr, spin_until_cancelled, &spin,
                           DFTU_JOB_BLOCKING, &t) == DFTU_OK);
        std::atomic<dftu_status> ended{DFTU_E_UNKNOWN};
        svc->retain(nullptr, t);
        dstd::Thread runner([&] {
            test_plugin_common::Ctx own;
            ended.store(svc->run(own.get(), nullptr, t));
        });
        while (!spin.started.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(svc->test(nullptr, t) == 0);
        // Two cancels on a running task the caller holds a reference to.
        svc->cancel(nullptr, t);
        svc->cancel(nullptr, t);
        runner.join();
        svc->release(nullptr, t);
        CHECK(ended.load() == DFTU_E_CANCELLED);
        CHECK(spin.runs.load() == 1);
    }

    TEST_CASE("the last release frees the task, whoever holds it") {
        const dftu_svc_task* svc = task_service();
        std::atomic<int> ran{0};
        dftu_task* t = plugins::task_to_abi(set_flag(&ran));
        svc->retain(nullptr, t);
        svc->retain(nullptr, t);
        svc->release(nullptr, t);
        CHECK(run_task(t) == DFTU_OK);
        CHECK(svc->test(nullptr, t) == 1);
        svc->cancel(nullptr, t);
        svc->release(nullptr, t);
        // The memory exit check fails the process if the task leaks.
    }

    TEST_CASE("a cancel racing a natural finish and the owner's free is safe") {
        const dftu_svc_task* svc = task_service();
        constexpr int TASKS = 2000;
        std::atomic<int> ran{0};
        for (int i = 0; i < TASKS; ++i) {
            dftu_task* t = plugins::task_to_abi(set_flag(&ran));
            svc->retain(nullptr, t);
            std::atomic<bool> go{false};
            dstd::Thread canceller([&] {
                while (!go.load()) std::this_thread::yield();
                for (int spin = i % 64; spin > 0; --spin) {
                    std::this_thread::yield();
                }
                svc->cancel(nullptr, t);
                svc->release(nullptr, t);
            });
            go.store(true);
            const dftu_status st = run_task(t);
            CHECK((st == DFTU_OK || st == DFTU_E_CANCELLED));
            canceller.join();
        }
        CHECK(ran.load() <= TASKS);
    }

    TEST_CASE("cancel before the task runs ends it as cancelled unrun") {
        Spin spin;
        const dftu_svc_task* svc = task_service();
        test_plugin_common::Ctx ctx;
        dftu_task* t = nullptr;
        REQUIRE(svc->spawn(ctx.get(), nullptr, spin_until_cancelled, &spin,
                           DFTU_JOB_BLOCKING, &t) == DFTU_OK);
        svc->cancel(nullptr, t);
        svc->cancel(nullptr, t);
        CHECK(run_task(t) == DFTU_E_CANCELLED);
        CHECK(spin.runs.load() == 0);
    }

    TEST_CASE(
        "cancel of an outer task reaches an inner task parked in a recv") {
        plugins::host::ChannelDecl decl;
        decl.name = "plugin.t.q";
        decl.credits = 4;
        decl.overflow = DFTU_OVERFLOW_FAIL;
        decl.max_msg_bytes = 16;
        plugins::host::PortChannel channel(decl);
        char buf[16];
        std::uint32_t len = 0;
        dftu_port_status status = DFTU_PORT_OK;
        dftu_task* inner = plugins::make_abi_task(
            dstd::Vector<std::byte>{},
            [&](plugins::AbiTask*) {
                return channel.recv_now(buf, sizeof buf, &len, &status, false);
            },
            [&](plugins::AbiTask*) {
                return channel.recv(buf, sizeof buf, &len, &status, false);
            });
        std::atomic<int> ended{0};
        dftu_status inner_status = DFTU_E_UNKNOWN;
        dftu_task* outer = plugins::task_to_abi(
            run_inner(plugins::abi_task(inner), &ended, &inner_status));
        std::atomic<bool> done{false};
        plugins::retain_abi_task(outer);
        dstd::Thread runner([&] {
            run_task(outer);
            done.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        plugins::cancel_abi_task(outer);
        const bool stopped = wait_for(done);
        if (!stopped) plugins::cancel_abi_task(inner);
        runner.join();
        plugins::free_abi_task(outer);
        CHECK(stopped);
        CHECK(ended.load() == 1);
        CHECK(inner_status == DFTU_E_CANCELLED);
        plugins::free_abi_task(inner);
    }

    TEST_CASE("cancel stops a task that waits on an async generator") {
        coro::AsyncSemaphore sem(1);
        REQUIRE(sem.try_acquire(1));
        std::atomic<int> yielded{0};
        dftu_task* t = plugins::task_to_abi(read_one(&sem, &yielded));
        std::atomic<bool> done{false};
        std::atomic<dftu_status> status{DFTU_E_UNKNOWN};
        plugins::retain_abi_task(t);
        dstd::Thread runner([&] {
            status.store(run_task(t));
            done.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        plugins::cancel_abi_task(t);
        const bool stopped = wait_for(done);
        if (!stopped) sem.release(1);
        runner.join();
        plugins::free_abi_task(t);
        CHECK(stopped);
        CHECK(status.load() == DFTU_E_CANCELLED);
        CHECK(yielded.load() == 0);
    }
}
