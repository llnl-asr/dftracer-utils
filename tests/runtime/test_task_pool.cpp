#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
namespace memory = dftracer::utils::memory;

namespace {

constexpr std::size_t BLOCK = 256;
// Past the per-thread batching bound, so it reaches the pool counters at once
// and stands out from anything else the general pool holds.
constexpr std::size_t BIG_BLOCK = std::size_t{3} << 19;
constexpr std::size_t ALIGN = 16;
constexpr int MAX_HOPS = 200;
constexpr int ROUNDS = 16;

// Resumes the coroutine from a plain thread that has a scope of its own, as an
// I/O completion or a timer does: the pool must come from the task, not from
// the thread that enqueues it.
struct Hops {
    memory::Pool* enqueuer_pool = nullptr;
    std::mutex mutex;
    std::vector<std::thread> threads;

    void join() {
        for (std::thread& t : threads) t.join();
        threads.clear();
    }
};

struct ResumeFromThread {
    Hops& hops;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        Executor* exec = Executor::current();
        if (exec == nullptr) std::terminate();
        std::lock_guard<std::mutex> lock(hops.mutex);
        hops.threads.emplace_back([exec, h, pool = hops.enqueuer_pool] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            memory::Scope scope{*pool};
            exec->enqueue(h);
        });
    }
    void await_resume() const noexcept {}
};

struct Blocks {
    std::size_t size = BLOCK;
    std::vector<void*> held;
    bool moved = false;

    void free_all() {
        for (void* p : held) memory::deallocate(p, size);
        held.clear();
    }
};

CoroTask<void> allocate_across_workers(Blocks* out, Hops* hops) {
    out->held.push_back(memory::allocate(out->size, ALIGN));
    const std::thread::id first = std::this_thread::get_id();
    for (int i = 0; i < MAX_HOPS && std::this_thread::get_id() == first; ++i)
        co_await ResumeFromThread{*hops};
    out->moved = std::this_thread::get_id() != first;
    out->held.push_back(memory::allocate(out->size, ALIGN));
}

CoroTask<void> allocate_and_yield(Blocks* out, std::set<std::thread::id>* seen,
                                  std::mutex* seen_mutex) {
    for (int i = 0; i < ROUNDS; ++i) {
        out->held.push_back(memory::allocate(out->size, ALIGN));
        {
            std::lock_guard<std::mutex> lock(*seen_mutex);
            seen->insert(std::this_thread::get_id());
        }
        co_await yield();
    }
}

CoroTask<void> allocate_and_yield_once(Blocks* out) {
    out->held.push_back(memory::allocate(out->size, ALIGN));
    co_await yield();
    out->held.push_back(memory::allocate(out->size, ALIGN));
}

CoroTask<void> hop_then_allocate(Blocks* out, Hops* hops) {
    co_await ResumeFromThread{*hops};
    out->held.push_back(memory::allocate(out->size, ALIGN));
}

CoroTask<void> parent_with_children(Blocks* direct, Blocks* spawned, Hops* hops,
                                    Executor* exec) {
    co_await hop_then_allocate(direct, hops);
    co_await run_coro_scope(exec, [spawned, hops](CoroScope& s) {
        s.spawn([spawned, hops](CoroScope&) -> CoroTask<void> {
            co_await hop_then_allocate(spawned, hops);
        });
        return s.join();
    });
}

memory::PoolHandle open_pool(const char* name) {
    memory::PoolHandle pool = memory::Manager::get().root().child(name);
    REQUIRE(pool);
    return pool;
}

template <typename Make>
auto under(memory::Pool& pool, Make make) {
    memory::Scope scope{pool};
    return make();
}

}  // namespace

TEST_CASE(
    "a task that moves to another worker charges both blocks to its pool") {
    Runtime rt(4);
    memory::PoolHandle pool = open_pool("task");
    memory::PoolHandle enqueuer = open_pool("enqueuer");
    Hops hops;
    hops.enqueuer_pool = enqueuer.get();
    Blocks blocks;

    CoroTask<void> task =
        under(*pool, [&] { return allocate_across_workers(&blocks, &hops); });
    rt.submit(std::move(task)).get();
    hops.join();

    REQUIRE(blocks.moved);
    CHECK(pool->blocks() == 2);
    CHECK(pool->live() >= 2 * BLOCK);
    CHECK(enqueuer->blocks() == 0);
    blocks.free_all();
    CHECK(pool->live() == 0);
}

TEST_CASE("two tasks with different pools on one worker do not mix") {
    Runtime rt(1);
    memory::PoolHandle pool_a = open_pool("a");
    memory::PoolHandle pool_b = open_pool("b");
    Blocks blocks_a;
    Blocks blocks_b;
    std::set<std::thread::id> seen;
    std::mutex seen_mutex;

    CoroTask<void> a = under(*pool_a, [&] {
        return allocate_and_yield(&blocks_a, &seen, &seen_mutex);
    });
    CoroTask<void> b = under(*pool_b, [&] {
        return allocate_and_yield(&blocks_b, &seen, &seen_mutex);
    });
    TaskHandle ha = rt.submit(std::move(a));
    TaskHandle hb = rt.submit(std::move(b));
    ha.get();
    hb.get();

    CHECK(seen.size() == 1);
    CHECK(pool_a->blocks() == ROUNDS);
    CHECK(pool_b->blocks() == ROUNDS);
    blocks_a.free_all();
    blocks_b.free_all();
    CHECK(pool_a->live() == 0);
    CHECK(pool_b->live() == 0);
}

TEST_CASE("a task made without a scope charges the general pool") {
    Runtime rt(2);
    memory::Pool& general = memory::Manager::get().general();
    memory::PoolHandle submitter = open_pool("submitter");
    memory::PoolHandle enqueuer = open_pool("enqueuer");
    Hops hops;
    hops.enqueuer_pool = enqueuer.get();
    Blocks blocks;
    blocks.size = BIG_BLOCK;

    const std::size_t before = general.live();
    CoroTask<void> task = hop_then_allocate(&blocks, &hops);
    {
        memory::Scope scope{*submitter};
        rt.submit(std::move(task)).get();
    }
    hops.join();

    REQUIRE(blocks.held.size() == 1);
    const std::size_t added = general.live() - before;
    CHECK(added >= BIG_BLOCK);
    CHECK(added < 2 * BIG_BLOCK);
    CHECK(submitter->blocks() == 0);
    CHECK(enqueuer->blocks() == 0);
    blocks.free_all();
}

TEST_CASE("children awaited or spawned inside a task take its pool") {
    Runtime rt(4);
    memory::PoolHandle pool = open_pool("parent");
    memory::PoolHandle enqueuer = open_pool("enqueuer");
    Hops hops;
    hops.enqueuer_pool = enqueuer.get();
    Blocks direct;
    Blocks spawned;

    CoroTask<void> task = under(*pool, [&] {
        return parent_with_children(&direct, &spawned, &hops, rt.executor());
    });
    rt.submit(std::move(task)).get();
    hops.join();

    REQUIRE(direct.held.size() == 1);
    REQUIRE(spawned.held.size() == 1);
    CHECK(pool->blocks() == 2);
    CHECK(enqueuer->blocks() == 0);
    direct.free_all();
    spawned.free_all();
    CHECK(pool->live() == 0);
}

TEST_CASE("get() charges the task's pool and keeps the caller's scope") {
    memory::PoolHandle pool = open_pool("task");
    memory::PoolHandle caller = open_pool("caller");
    Blocks blocks;

    {
        CoroTask<void> task =
            under(*pool, [&] { return allocate_and_yield_once(&blocks); });
        memory::Scope scope{*caller};
        task.get();
        CHECK(memory::current_pool().id() == caller->id());
    }

    CHECK(pool->blocks() == 2);
    CHECK(caller->blocks() == 0);
    blocks.free_all();
}

TEST_CASE("a task whose pool closed before it ran charges the general pool") {
    memory::Pool& general = memory::Manager::get().general();
    // A Results pool may close with the task's frame still live.
    memory::PoolHandle pool = memory::Manager::get().root().child(
        "closed", memory::PoolKind::Results);
    REQUIRE(pool);
    Blocks blocks;
    blocks.size = BIG_BLOCK;

    CoroTask<void> task =
        under(*pool, [&] { return allocate_and_yield_once(&blocks); });
    pool.close();
    const std::size_t before = general.live();
    task.get();

    const std::size_t added = general.live() - before;
    CHECK(added >= 2 * BIG_BLOCK);
    CHECK(added < 3 * BIG_BLOCK);
    blocks.free_all();
}
