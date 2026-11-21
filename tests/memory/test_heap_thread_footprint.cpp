// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/heap_page.h>
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
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
