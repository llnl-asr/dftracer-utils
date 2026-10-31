#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/coro/async_mutex.h>
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/coro/when_all.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace dftracer::utils;

namespace {

struct StackSpan {
    std::uintptr_t lowest = UINTPTR_MAX;
    std::uintptr_t highest = 0;
    void record(std::uintptr_t a) {
        lowest = std::min(lowest, a);
        highest = std::max(highest, a);
    }
};

// An address on the current thread's stack. GCC keeps every local of a
// coroutine in its heap frame, so a local of the waiter itself would measure
// how far apart the frames are, not the stack depth.
[[gnu::noinline]] std::uintptr_t stack_address() {
    return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
}

coro::CoroTask<void> waiter(coro::AsyncMutex& mutex, StackSpan& span,
                            std::size_t& acquired) {
    co_await mutex.lock();
    span.record(stack_address());
    ++acquired;
    mutex.unlock();
}

coro::CoroTask<void> releaser(coro::AsyncMutex& mutex) {
    mutex.unlock();
    co_return;
}

}  // namespace

TEST_SUITE("AsyncMutex") {
    TEST_CASE(
        "an unlock hands off to many same-executor waiters at a fixed "
        "stack depth") {
        // One thread, so every waiter shares the unlocker's executor and the
        // hand-off takes the same-executor path that once resumed each waiter
        // inside the previous one's unlock.
        constexpr std::size_t WAITERS = 20000;
        constexpr std::uintptr_t MAX_SPAN = 64 * 1024;

        coro::AsyncMutex mutex;
        StackSpan span;
        std::size_t acquired = 0;

        Runtime rt(1);
        auto body = [&](CoroScope&) -> coro::CoroTask<void> {
            co_await mutex.lock();
            // Every waiter queues before the releaser, which starts last.
            std::vector<coro::CoroTask<void>> tasks;
            tasks.reserve(WAITERS + 1);
            for (std::size_t i = 0; i < WAITERS; ++i) {
                tasks.push_back(waiter(mutex, span, acquired));
            }
            tasks.push_back(releaser(mutex));
            co_await coro::when_all(std::move(tasks));
        };
        rt.submit(run_coro_scope(rt.executor(), body), "MutexChain").wait();
        rt.shutdown();

        CHECK(acquired == WAITERS);
        CHECK(span.highest - span.lowest < MAX_SPAN);
    }
}
