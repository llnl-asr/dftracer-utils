#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/reserve_async.h>
#include <dftracer/utils/runtime/coro/yield.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <random>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
namespace memory = dftracer::utils::memory;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t MIB = std::size_t{1} << 20;
// Past the per-thread batching bound, so it reaches the pool counters at once.
constexpr std::size_t BIG_BLOCK = std::size_t{3} << 19;
constexpr std::size_t ALIGN = 16;

using Clock = std::chrono::steady_clock;

// Construct it after the Runtime: the first Runtime applies the memory
// configuration from the environment.
struct Budget {
    explicit Budget(std::size_t bytes) {
        memory::Config c;
        c.budget = bytes;
        memory::configure(c);
    }
    ~Budget() { memory::configure(memory::Config{}); }
};

memory::Pool& root() { return memory::Manager::get().root(); }

memory::PoolHandle open_pool(const char* name) {
    memory::PoolHandle pool = root().child(name);
    REQUIRE(pool);
    return pool;
}

template <typename Make>
auto under(memory::Pool& pool, Make make) {
    memory::Scope scope{pool};
    return make();
}

struct Outcome {
    memory::Status status = memory::Status::Pending;
    std::size_t bytes = 0;
    Executor* resumed_on = nullptr;
    std::thread::id thread;
    Clock::duration waited{};
    void* block = nullptr;
};

CoroTask<void> wait_for(memory::Pool* pool, std::size_t bytes,
                        Clock::time_point deadline, Outcome* out,
                        std::atomic<bool>* about_to_wait) {
    if (about_to_wait != nullptr) {
        about_to_wait->store(true, std::memory_order_release);
    }
    const Clock::time_point start = Clock::now();
    ReserveAsync::Result r = co_await reserve_async(*pool, bytes, deadline);
    out->waited = Clock::now() - start;
    out->resumed_on = Executor::current();
    out->thread = std::this_thread::get_id();
    if (r) {
        out->status = memory::Status::Ok;
        out->bytes = r->bytes();
        out->block = memory::allocate(BIG_BLOCK, ALIGN);
    } else {
        out->status = r.error();
    }
}

CoroTask<void> release_later(memory::Reservation* held,
                             std::atomic<bool>* about_to_wait) {
    while (!about_to_wait->load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(20ms);
    held->release();
    co_return;
}

struct Bare {
    struct promise_type {
        Bare get_return_object() {
            return Bare{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
    std::coroutine_handle<promise_type> handle;
};

struct Alive {
    std::atomic<int>* count;
    explicit Alive(std::atomic<int>* c) : count(c) { count->fetch_add(1); }
    ~Alive() { count->fetch_sub(1); }
};

Bare wait_in_bare(memory::Pool* pool, std::size_t bytes,
                  Clock::time_point deadline, std::atomic<int>* alive,
                  std::atomic<bool>* resumed) {
    Alive guard{alive};
    ReserveAsync::Result r = co_await reserve_async(*pool, bytes, deadline);
    resumed->store(true);
    (void)r;
}

CoroTask<void> start_and_destroy(memory::Pool* pool, std::size_t bytes,
                                 Clock::time_point deadline,
                                 std::atomic<int>* alive,
                                 std::atomic<bool>* resumed, bool* suspended) {
    Bare bare = wait_in_bare(pool, bytes, deadline, alive, resumed);
    *suspended = !bare.handle.done();
    bare.handle.destroy();
    co_return;
}

}  // namespace

TEST_CASE("a waiter with room gets its bytes without waiting") {
    Runtime rt(2);
    Budget budget(64 * MIB);
    memory::PoolHandle pool = open_pool("task");
    Outcome out;
    rt.submit(wait_for(pool.get(), 8 * MIB, Clock::now() + 10s, &out, nullptr))
        .get();
    CHECK(out.status == memory::Status::Ok);
    CHECK(out.bytes == 8 * MIB);
    CHECK(root().reserved() == 0);
    memory::deallocate(out.block, BIG_BLOCK);
}

TEST_CASE("a request that can never fit fails at once") {
    Runtime rt(2);
    Budget budget(64 * MIB);
    memory::PoolHandle pool = open_pool("task");
    Outcome out;
    auto start = Clock::now();
    rt.submit(
          wait_for(pool.get(), 128 * MIB, Clock::now() + 10s, &out, nullptr))
        .get();
    CHECK(out.status == memory::Status::OverBudget);
    CHECK(Clock::now() - start < 5s);
}

TEST_CASE("a waiter resumes on its executor when another task releases") {
    Runtime rt(2);
    Budget budget(64 * MIB);
    memory::PoolHandle holds = open_pool("holds");
    memory::PoolHandle pool = open_pool("task");
    memory::Reservation held = holds->reserve(64 * MIB);
    REQUIRE(held);

    Outcome out;
    std::atomic<bool> about_to_wait{false};
    CoroTask<void> waiter = under(*pool, [&] {
        return wait_for(pool.get(), 32 * MIB, Clock::time_point::max(), &out,
                        &about_to_wait);
    });
    TaskHandle w = rt.submit(std::move(waiter));
    CoroTask<void> releaser =
        under(*holds, [&] { return release_later(&held, &about_to_wait); });
    rt.submit(std::move(releaser)).get();
    w.get();

    CHECK(out.status == memory::Status::Ok);
    CHECK(out.bytes == 32 * MIB);
    CHECK(out.waited >= 10ms);
    CHECK(out.resumed_on == rt.executor());
    CHECK(pool->blocks() == 1);
    CHECK(holds->blocks() == 0);
    CHECK(root().reserved() == 0);
    memory::deallocate(out.block, BIG_BLOCK);
}

TEST_CASE("a release from a plain thread does not resume the waiter inline") {
    Runtime rt(2);
    Budget budget(64 * MIB);
    memory::PoolHandle holds = open_pool("holds");
    memory::PoolHandle pool = open_pool("task");
    memory::Reservation held = holds->reserve(64 * MIB);
    REQUIRE(held);

    Outcome out;
    std::atomic<bool> about_to_wait{false};
    TaskHandle w = rt.submit(wait_for(pool.get(), 16 * MIB, Clock::now() + 30s,
                                      &out, &about_to_wait));
    std::thread::id releaser;
    std::thread t([&] {
        releaser = std::this_thread::get_id();
        while (!about_to_wait.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(20ms);
        held.release();
    });
    t.join();
    w.get();

    CHECK(out.status == memory::Status::Ok);
    CHECK(out.waited >= 10ms);
    CHECK(out.resumed_on == rt.executor());
    CHECK(out.thread != releaser);
    memory::deallocate(out.block, BIG_BLOCK);
}

TEST_CASE("a waiter times out and the later release skips it") {
    Runtime rt(2);
    Budget budget(64 * MIB);
    memory::PoolHandle holds = open_pool("holds");
    memory::PoolHandle pool = open_pool("task");
    memory::Reservation held = holds->reserve(64 * MIB);
    REQUIRE(held);

    Outcome out;
    auto start = Clock::now();
    rt.submit(
          wait_for(pool.get(), 32 * MIB, Clock::now() + 50ms, &out, nullptr))
        .get();
    CHECK(out.status == memory::Status::OverBudget);
    CHECK(Clock::now() - start >= 50ms);
    CHECK(out.resumed_on == rt.executor());

    held.release();
    CHECK(root().reserved() == 0);
    CHECK(pool->reserved() == 0);
}

TEST_CASE("a blocking get() on its own run loop does not wait") {
    Budget budget(64 * MIB);
    memory::PoolHandle holds = open_pool("holds");
    memory::PoolHandle pool = open_pool("task");
    memory::Reservation held = holds->reserve(64 * MIB);
    REQUIRE(held);

    Outcome out;
    CoroTask<void> task =
        wait_for(pool.get(), 32 * MIB, Clock::now() + 10s, &out, nullptr);
    CHECK_NOTHROW(task.get());
    CHECK(out.status != memory::Status::Ok);
    CHECK(out.waited < 5s);
    CHECK(root().reserved() == 64 * MIB);
}

TEST_CASE("a frame destroyed while it waits leaks nothing") {
    Runtime rt(2);
    Budget budget(64 * MIB);
    memory::PoolHandle holds = open_pool("holds");
    memory::PoolHandle pool = open_pool("task");
    memory::Reservation held = holds->reserve(64 * MIB);
    REQUIRE(held);

    std::atomic<int> alive{0};
    std::atomic<bool> resumed{false};
    bool suspended = false;
    memory::Pool& general = memory::Manager::get().general();
    std::atomic<bool> warm{false};
    rt.submit(start_and_destroy(pool.get(), 0, Clock::now() + 50ms, &alive,
                                &warm, &suspended))
        .get();
    REQUIRE(warm.load());
    const std::size_t before = general.blocks();
    rt.submit(start_and_destroy(pool.get(), 32 * MIB, Clock::now() + 50ms,
                                &alive, &resumed, &suspended))
        .get();
    CHECK(suspended);
    CHECK(alive.load() == 0);

    held.release();
    std::this_thread::sleep_for(100ms);
    CHECK(!resumed.load());
    CHECK(root().reserved() == 0);
    // The warm-up frame may still be counted in `before`: a worker destroys
    // it just after get() returns. A leak can only add blocks.
    CHECK(general.blocks() <= before);
}

TEST_CASE("many waiters on several workers end exact") {
    constexpr int TASKS = 32;
    constexpr int ROUNDS = 20;
    constexpr std::size_t SIZES[] = {8 * MIB, 16 * MIB, 32 * MIB, 48 * MIB};
    Runtime rt(4);
    Budget budget(64 * MIB);
    std::vector<memory::PoolHandle> pools;
    for (int i = 0; i < TASKS; ++i) pools.push_back(open_pool("t"));

    std::atomic<int> granted{0};
    std::atomic<int> timed_out{0};
    std::atomic<int> bad{0};
    auto worker = [&](memory::Pool* pool, unsigned seed) -> CoroTask<void> {
        std::mt19937_64 rng(seed);
        for (int i = 0; i < ROUNDS; ++i) {
            std::size_t size = SIZES[rng() % 4];
            Clock::time_point deadline =
                rng() % 4 == 0
                    ? Clock::time_point::max()
                    : Clock::now() + std::chrono::milliseconds(1 + rng() % 5);
            ReserveAsync::Result r =
                co_await reserve_async(*pool, size, deadline);
            if (Executor::current() == nullptr) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
            if (r) {
                if (r->bytes() != size) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
                granted.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(200us);
                co_await yield();
            } else if (r.error() == memory::Status::OverBudget) {
                timed_out.fetch_add(1, std::memory_order_relaxed);
            } else {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    std::vector<TaskHandle> handles;
    for (int i = 0; i < TASKS; ++i) {
        handles.push_back(
            rt.submit(worker(pools[static_cast<std::size_t>(i)].get(),
                             static_cast<unsigned>(100 + i))));
    }
    for (TaskHandle& h : handles) h.get();

    CHECK(bad.load() == 0);
    CHECK(granted.load() + timed_out.load() == TASKS * ROUNDS);
    CHECK(granted.load() > 0);
    CHECK(timed_out.load() > 0);
    CHECK(root().reserved() == 0);
}
