// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/heap_page.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

using namespace dftracer::utils::memory::detail;

namespace {

constexpr std::size_t MIB = std::size_t{1} << 20;

// What a pipeline stage does on a worker: blocks of many sizes held together,
// then all freed, and the thread goes idle without freeing again.
void burst() {
    std::vector<void*> blocks;
    std::size_t held = 0;
    while (held < 12 * MIB) {
        for (std::size_t size = 4096; size <= 256 * 1024; size *= 2) {
            blocks.push_back(page_allocate(size, 16));
            held += size;
        }
    }
    for (void* p : blocks) page_deallocate(p);
}

// Freed bytes still backed while `threads` threads sit idle after a burst.
std::size_t idle_after(unsigned threads) {
    std::atomic<unsigned> done{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            burst();
            // A busy thread keeps freeing; one small free later is what lets
            // its heap give back what has sat idle.
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            page_deallocate(page_allocate(128 * 1024, 16));
            done.fetch_add(1);
            while (!release.load()) std::this_thread::yield();
        });
    while (done.load() < threads) std::this_thread::yield();
    const std::size_t idle = page_idle_backed();
    release.store(true);
    for (auto& t : pool) t.join();
    return idle;
}

}  // namespace

TEST_CASE("freed memory that stays backed is bounded, not a share per thread") {
    const std::size_t one = idle_after(1);
    const std::size_t many = idle_after(16);
    constexpr std::size_t BOUND = 48 * MIB;
    CHECK(one <= BOUND);
    CHECK(many <= BOUND);
}

TEST_CASE("a thread that exits gives back the idle pages of its heap") {
    constexpr std::size_t size = HEAP_PAGE_SIZE + 5000;
    constexpr std::size_t count = 64;
    std::vector<void*> kept;
    std::size_t idle_before_exit = 0;
    std::thread worker([&] {
        std::vector<void*> blocks;
        for (std::size_t i = 0; i < count; ++i) {
            blocks.push_back(page_allocate(size, 16));
            std::memset(blocks.back(), 1, size);
        }
        // Every other block stays live, so the freed runs stay in this heap.
        for (std::size_t i = 0; i < count; ++i) {
            if (i % 2 == 0)
                page_deallocate(blocks[i], size);
            else
                kept.push_back(blocks[i]);
        }
        idle_before_exit = page_idle_backed();
    });
    worker.join();
    const std::size_t idle_after_exit = page_idle_backed();
    for (void* p : kept) page_deallocate(p, size);
    CHECK(idle_before_exit >= count / 2 * HEAP_PAGE_SIZE);
    CHECK(idle_after_exit + count / 2 * HEAP_PAGE_SIZE <= idle_before_exit);
}

TEST_CASE(
    "a heap takes back the medium runs another thread freed before it takes a "
    "new run") {
    constexpr std::size_t size = 256 * 1024;
    constexpr std::size_t count = 8;
    // Idle bytes left by earlier cases could reach the cap, and its relief
    // would purge them between the two readings.
    page_trim(Mode::All);
    std::vector<void*> blocks;
    std::atomic<int> stage{0};
    std::size_t idle_before = 0;
    std::size_t idle_after = 0;
    std::thread owner([&] {
        void* live = page_allocate(size, 16);
        for (std::size_t i = 0; i < count; ++i) {
            blocks.push_back(page_allocate(size, 16));
            std::memset(blocks.back(), 1, size);
        }
        stage.store(1);
        while (stage.load() != 2) std::this_thread::yield();
        idle_before = page_idle_backed();
        void* fresh = page_allocate(128 * 1024, 16);
        idle_after = page_idle_backed();
        page_deallocate(fresh);
        page_deallocate(live);
        stage.store(3);
    });
    while (stage.load() != 1) std::this_thread::yield();
    for (void* p : blocks) page_deallocate(p);
    stage.store(2);
    owner.join();
    CHECK(idle_after >= idle_before + (count - 1) * size);
}
