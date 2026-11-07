// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/async_mutex.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/when_any.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <mutex>
#include <utility>

using namespace dftracer::utils;

namespace {

/// A user executor that implements the one required operation: it keeps the
/// handles it is given and the test thread runs them. A timer thread can post
/// too, so the queue takes a lock.
struct MiniExecutor : coro::Executor {
    void post(std::coroutine_handle<> handle) noexcept override {
        if (!handle) return;
        std::lock_guard<std::mutex> lock(mutex);
        ready.push_back(handle);
    }

    /// Runs the queued handles in the order they were posted, then destroys
    /// the frames that finished. Everything the tasks post while they run is
    /// picked up before it returns.
    void run() {
        for (;;) {
            const std::coroutine_handle<> handle = take();
            if (!handle) break;
            if (!handle.done()) coro::resume_on(*this, handle);
        }
        coro::drain_thread_local_destroys();
    }

    /// Runs like run(), parking this thread whenever the queue is empty, until
    /// `done` holds. Work another thread posts is picked up too.
    void run_until(const std::atomic<bool>& done) {
        while (!done.load(std::memory_order_acquire)) {
            const std::coroutine_handle<> handle = take();
            if (!handle) {
                coro::park_driver(&queue_filled, this,
                                  std::chrono::milliseconds{2});
                continue;
            }
            if (!handle.done()) coro::resume_on(*this, handle);
        }
        coro::drain_thread_local_destroys();
    }

    bool is_empty() {
        std::lock_guard<std::mutex> lock(mutex);
        return ready.empty();
    }

   private:
    std::coroutine_handle<> take() {
        std::lock_guard<std::mutex> lock(mutex);
        if (ready.empty()) return {};
        const std::coroutine_handle<> handle = ready.front();
        ready.erase(ready.begin());
        return handle;
    }

    static bool queue_filled(void* self) {
        return !static_cast<MiniExecutor*>(self)->is_empty();
    }

    std::mutex mutex;
    dstd::Vector<std::coroutine_handle<>> ready;
};

coro::CoroTask<int> leaf(int value) {
    co_await coro::yield();
    co_await coro::yield();
    co_return value;
}

/// Takes the mutex, awaits a chain of tasks while it holds it, and gives it
/// back. Two of these on one executor must not overlap.
coro::CoroTask<void> contender(coro::AsyncMutex& mutex, int& total,
                               std::atomic<int>& holding,
                               std::atomic<int>& peak) {
    co_await mutex.lock();
    const int now = holding.fetch_add(1) + 1;
    peak.store(std::max(peak.load(), now));
    total += co_await leaf(7);
    co_await coro::yield();
    holding.fetch_sub(1);
    mutex.unlock();
}

coro::CoroTask<void> noop() { co_return; }

/// Waits on a timeout the executor cannot serve itself, so the wait lands on
/// the process-wide timer thread.
coro::CoroTask<void> wait_beyond(std::atomic<bool>& timed_out) {
    try {
        co_await coro::timeout(std::chrono::milliseconds{20}, nullptr);
    } catch (const common::DFTUtilsException&) {
        timed_out.store(true, std::memory_order_release);
    }
}

/// The frame the driver owns: awaiting the task is the only thing that keeps
/// it alive, so the driver releases this handle once the coroutine is set up.
coro::detail::Coro drive(coro::CoroTask<void> task, std::atomic<bool>& done,
                         std::exception_ptr& error) {
    try {
        co_await std::move(task);
    } catch (...) {
        error = std::current_exception();
    }
    done.store(true, std::memory_order_release);
}

/// Blocks on a task from inside the executor's only thread.
coro::CoroTask<void> block_on_own_thread(MiniExecutor& exec, bool& threw,
                                         bool& left_queue_empty,
                                         bool& task_untouched) {
    auto inner = noop();
    try {
        inner.get();
    } catch (const common::DFTUtilsException&) {
        threw = true;
    }
    left_queue_empty = exec.is_empty();
    task_untouched = !inner.done();
    co_return;
}

struct CompletionSpy {
    std::atomic<int>* calls;
    std::atomic<common::TaskIndex>* last_id;

    static void on_complete(void* owner, common::TaskIndex id) noexcept {
        auto* self = static_cast<CompletionSpy*>(owner);
        self->last_id->store(id);
        self->calls->fetch_add(1);
    }
};

}  // namespace

TEST_SUITE("UserExecutor") {
    TEST_CASE(
        "an executor that only posts runs a coro chain and a mutex to "
        "completion") {
        MiniExecutor exec;
        coro::AsyncMutex mutex;
        int total = 0;
        std::atomic<int> holding{0};
        std::atomic<int> peak{0};
        std::atomic<bool> first_done{false};
        std::atomic<bool> second_done{false};
        std::exception_ptr first_error;
        std::exception_ptr second_error;

        auto first = drive(contender(mutex, total, holding, peak), first_done,
                           first_error);
        auto second = drive(contender(mutex, total, holding, peak), second_done,
                            second_error);
        exec.post(first.release());
        exec.post(second.release());
        exec.run();

        CHECK(first_done.load());
        CHECK(second_done.load());
        CHECK(first_error == nullptr);
        CHECK(second_error == nullptr);
        CHECK(total == 14);
        CHECK(peak.load() == 1);
        CHECK(holding.load() == 0);
    }

    TEST_CASE("a timeout with no executor timers uses the process timers") {
        MiniExecutor exec;
        std::atomic<bool> timed_out{false};
        std::atomic<bool> done{false};
        std::exception_ptr error;

        auto task = drive(wait_beyond(timed_out), done, error);
        exec.post(task.release());
        exec.run_until(done);

        CHECK(done.load());
        CHECK(timed_out.load());
        CHECK(error == nullptr);
    }

    TEST_CASE("a tracked task reports completion once, an untracked none") {
        MiniExecutor exec;
        std::atomic<int> calls{0};
        std::atomic<common::TaskIndex> last_id{-1};
        CompletionSpy spy{&calls, &last_id};
        coro::detail::TaskTracking tracking{&CompletionSpy::on_complete, &spy,
                                            4242};

        std::atomic<bool> done{false};
        std::exception_ptr error;
        auto tracked = drive(noop(), done, error);
        tracked.handle().promise().tracking = &tracking;
        exec.post(tracked.release());
        exec.run();

        CHECK(done.load());
        CHECK(error == nullptr);
        CHECK(calls.load() == 1);
        CHECK(last_id.load() == 4242);

        // A task with no tracker keeps the count where the tracked one left
        // it, and its promise says so.
        done.store(false);
        auto untracked = drive(noop(), done, error);
        CHECK(untracked.handle().promise().tracking == nullptr);
        exec.post(untracked.release());
        exec.run();

        CHECK(done.load());
        CHECK(calls.load() == 1);
    }

    TEST_CASE("blocking on the only thread of a post-only executor raises") {
        MiniExecutor exec;
        bool threw = false;
        bool left_queue_empty = false;
        bool task_untouched = false;
        std::atomic<bool> done{false};
        std::exception_ptr error;

        auto task = drive(
            block_on_own_thread(exec, threw, left_queue_empty, task_untouched),
            done, error);
        exec.post(task.release());
        exec.run();

        CHECK(done.load());
        CHECK(error == nullptr);
        CHECK(threw);
        CHECK(left_queue_empty);
        CHECK(task_untouched);
    }
}
