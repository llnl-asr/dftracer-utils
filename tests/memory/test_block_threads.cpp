#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/block.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

using namespace dftracer::utils::memory;

namespace {

constexpr int THREADS = 8;
constexpr int ROUNDS = 20000;
constexpr std::size_t SIZE = 4096;

Pool& root() { return Manager::get().root(); }

std::uint8_t pattern(std::size_t i) {
    return static_cast<std::uint8_t>(i * 31);
}

// Each thread writes its own slot without synchronization and then drops its
// reference; release reads every slot, which is a race unless the last drop
// orders the writes before it.
struct Shared {
    std::uint64_t slots[THREADS] = {};
    std::uint64_t sum = 0;
    std::atomic<int> releases{0};
};

void sum_slots(void* context) {
    auto* shared = static_cast<Shared*>(context);
    for (std::uint64_t v : shared->slots) shared->sum += v;
    shared->releases.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

TEST_CASE("many threads copy and drop references to one owned block") {
    PoolHandle pool = root().child("shared_owned");
    REQUIRE(pool);
    BlockRef block;
    {
        Scope s{*pool};
        block = Block::allocate(SIZE);
    }
    std::uint8_t* bytes = block.mutable_data();
    for (std::size_t i = 0; i < SIZE; ++i) bytes[i] = pattern(i);

    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&bad, view = BufferView(block), t] {
            for (int r = 0; r < ROUNDS; ++r) {
                std::size_t at = static_cast<std::size_t>(r * 7 + t) % SIZE;
                BufferView part = view.slice(at, SIZE - at);
                BlockRef copy = part.block();
                BufferView inner = std::move(part).slice(0, 1);
                if (inner.data()[0] != pattern(at)) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    // Drop the main reference while the workers run, so one of them frees.
    block.reset();
    for (std::thread& th : threads) th.join();
    CHECK(bad.load() == 0);
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE(
    "the last of many threads to drop a borrowed block releases it once") {
    for (int trial = 0; trial < 50; ++trial) {
        Shared shared;
        std::uint8_t foreign[64] = {};
        BlockRef block =
            Block::borrow(foreign, sizeof(foreign), sum_slots, &shared);
        std::vector<std::thread> threads;
        for (int t = 0; t < THREADS; ++t) {
            threads.emplace_back([&shared, ref = block, t]() mutable {
                for (int r = 0; r < 200; ++r) {
                    BlockRef copy = ref;
                    copy.reset();
                }
                shared.slots[t] = static_cast<std::uint64_t>(t) + 1;
                ref.reset();
            });
        }
        block.reset();
        for (std::thread& th : threads) th.join();
        CHECK(shared.releases.load() == 1);
        CHECK(shared.sum == THREADS * (THREADS + 1) / 2);
    }
}
