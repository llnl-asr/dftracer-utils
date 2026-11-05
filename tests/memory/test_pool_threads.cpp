// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/detail/heap.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace dftracer::utils::memory;
using namespace dftracer::utils::memory::detail;

namespace {

constexpr std::size_t KIB = std::size_t{1} << 10;
constexpr std::size_t MIB = std::size_t{1} << 20;

struct Item {
    void* p;
    std::size_t size;
};

class Mailbox {
   public:
    void push(Item item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(item);
        }
        ready_.notify_one();
    }
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }
    bool pop(Item& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop_front();
        return true;
    }

   private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Item> queue_;
    bool closed_ = false;
};

Pool& root() { return Manager::get().root(); }

std::size_t random_size(std::mt19937_64& rng) {
    std::size_t roll = rng() % 1000;
    if (roll < 800) return 1 + rng() % (32 * KIB);
    if (roll < 990) return 32 * KIB + 1 + rng() % (MIB - 32 * KIB);
    return MIB + 1 + rng() % (3 * MIB);
}

}  // namespace

TEST_CASE("blocks freed on other threads credit the pool that was charged") {
    constexpr int THREADS = 6;
    constexpr int PER_THREAD = 3000;
    std::vector<PoolHandle> pools;
    for (int i = 0; i < THREADS; ++i) pools.push_back(root().child("worker"));
    std::size_t before = root().live();
    std::size_t before_blocks = root().blocks();

    std::vector<std::vector<Item>> made(THREADS);
    std::vector<std::size_t> asked(THREADS, 0);
    {
        std::vector<std::thread> producers;
        for (int t = 0; t < THREADS; ++t) {
            producers.emplace_back([&, t] {
                std::mt19937_64 rng(1000 + static_cast<unsigned>(t));
                Scope scope{*pools[static_cast<std::size_t>(t)]};
                for (int i = 0; i < PER_THREAD; ++i) {
                    std::size_t size = random_size(rng);
                    made[static_cast<std::size_t>(t)].push_back(
                        {allocate(size, 16), size});
                    asked[static_cast<std::size_t>(t)] += size;
                }
            });
        }
        for (std::thread& th : producers) th.join();
    }
    std::size_t total = 0;
    for (int t = 0; t < THREADS; ++t) {
        auto i = static_cast<std::size_t>(t);
        CHECK(pools[i]->blocks() == PER_THREAD);
        CHECK(pools[i]->live() >= asked[i]);
        total += pools[i]->live();
    }
    CHECK(root().live() == before + total);
    CHECK(root().blocks() == before_blocks + THREADS * PER_THREAD);

    {
        std::vector<std::thread> consumers;
        for (int t = 0; t < THREADS; ++t) {
            consumers.emplace_back([&, t] {
                // Each thread frees the blocks of the next thread's pool.
                auto owner = static_cast<std::size_t>((t + 1) % THREADS);
                for (const Item& item : made[owner]) deallocate(item.p);
            });
        }
        for (std::thread& th : consumers) th.join();
    }
    for (const PoolHandle& p : pools) {
        CHECK(p->blocks() == 0);
        CHECK(p->live() == 0);
    }
    CHECK(root().live() == before);
    CHECK(root().blocks() == before_blocks);
}

TEST_CASE("producers and consumers running together end exact") {
    constexpr int PAIRS = 4;
    constexpr int PER_PRODUCER = 20000;
    std::vector<PoolHandle> pools;
    for (int i = 0; i < PAIRS; ++i) pools.push_back(root().child("pair"));
    std::size_t before = root().live();
    std::size_t before_blocks = root().blocks();

    std::vector<Mailbox> boxes(PAIRS);
    std::vector<std::thread> threads;
    for (int t = 0; t < PAIRS; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937_64 rng(77 + static_cast<unsigned>(t));
            Scope scope{*pools[static_cast<std::size_t>(t)]};
            for (int i = 0; i < PER_PRODUCER; ++i) {
                std::size_t size = random_size(rng);
                boxes[static_cast<std::size_t>(t)].push(
                    {allocate(size, 16), size});
            }
            boxes[static_cast<std::size_t>(t)].close();
        });
        threads.emplace_back([&, t] {
            // A consumer with its own scope still credits the producer's pool.
            Scope scope{*pools[static_cast<std::size_t>((t + 1) % PAIRS)]};
            Item item;
            while (boxes[static_cast<std::size_t>(t)].pop(item)) {
                deallocate(item.p);
            }
        });
    }
    for (std::thread& th : threads) th.join();

    for (const PoolHandle& p : pools) {
        CHECK(p->blocks() == 0);
        CHECK(p->live() == 0);
    }
    CHECK(root().live() == before);
    CHECK(root().blocks() == before_blocks);
}

TEST_CASE("pools opened and closed on many threads end with clean slots") {
    constexpr int THREADS = 8;
    constexpr int ROUNDS = 400;
    std::size_t before = root().live();
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < ROUNDS; ++i) {
                PoolHandle pool = root().child("churn");
                if (!pool) {
                    ++failures;
                    continue;
                }
                void* p;
                {
                    Scope scope{*pool};
                    p = allocate(256, 16);
                }
                if (pool->live() == 0) ++failures;
                deallocate(p);
                if (pool->blocks() != 0) ++failures;
            }
        });
    }
    for (std::thread& th : threads) th.join();
    CHECK(failures.load() == 0);
    CHECK(root().live() == before);
}

// Blocks freed on another thread leave a charge in the cache of each thread.
// The owners flush those charges while the pool closes, and the close must not
// report a leak from a read that saw a charge neither in the cache nor in the
// total.
TEST_CASE("closing a pool while threads flush their charges reports no leak") {
    constexpr int WORKERS = 6;
    constexpr std::chrono::seconds RUN_FOR{5};
    constexpr std::size_t EVICTORS = 64;
    std::vector<PoolHandle> evictors;
    for (std::size_t i = 0; i < EVICTORS; ++i) {
        evictors.push_back(root().child("evictor", PoolKind::Results));
    }
    std::atomic<Pool*> current{nullptr};
    std::atomic<int> round{0};
    std::atomic<int> charged{0};
    std::atomic<bool> stop{false};
    std::vector<std::atomic<void*>> blocks(WORKERS);
    std::vector<std::thread> workers;
    for (int w = 0; w < WORKERS; ++w) {
        workers.emplace_back([&, w] {
            int seen = 0;
            std::size_t next = static_cast<std::size_t>(w);
            while (!stop.load(std::memory_order_acquire)) {
                const int now = round.load(std::memory_order_acquire);
                if (now != seen) {
                    seen = now;
                    Scope scope{*current.load(std::memory_order_acquire)};
                    blocks[static_cast<std::size_t>(w)].store(
                        allocate(64 + 64 * static_cast<std::size_t>(w), 16),
                        std::memory_order_release);
                    charged.fetch_add(1, std::memory_order_acq_rel);
                }
                Scope evict{*evictors[next++ % EVICTORS]};
                deallocate(allocate(64, 16), 64);
            }
        });
    }
    const auto end = std::chrono::steady_clock::now() + RUN_FOR;
    for (int r = 1; std::chrono::steady_clock::now() < end; ++r) {
        PoolHandle pool = root().child("flushed");
        current.store(pool.get(), std::memory_order_release);
        charged.store(0, std::memory_order_release);
        round.store(r, std::memory_order_release);
        while (charged.load(std::memory_order_acquire) != WORKERS) {
        }
        for (std::size_t w = 0; w < WORKERS; ++w) {
            deallocate(blocks[w].load(std::memory_order_acquire), 64 + 64 * w);
        }
        pool.close();
    }
    stop.store(true, std::memory_order_release);
    for (auto& t : workers) t.join();
    evictors.clear();
    CHECK_FALSE(Manager::get().leaked());
}
