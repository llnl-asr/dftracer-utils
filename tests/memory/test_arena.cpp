#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/arena.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

using namespace dftracer::utils::memory;

namespace {

constexpr std::size_t KIB = std::size_t{1} << 10;
constexpr std::size_t MIB = std::size_t{1} << 20;
constexpr std::size_t STEP = alignof(std::max_align_t);

constexpr std::size_t round_up(std::size_t n) {
    return (n + STEP - 1) / STEP * STEP;
}

Pool& root() { return Manager::get().root(); }

std::uintptr_t addr(const void* p) {
    return reinterpret_cast<std::uintptr_t>(p);
}

struct HardCap {
    explicit HardCap(std::size_t bytes) {
        Config c;
        c.hard_cap = bytes;
        configure(c);
    }
    ~HardCap() { configure(Config{}); }
};

std::vector<int> g_destroyed;

struct Tracked {
    explicit Tracked(int value) : id(value) {}
    ~Tracked() { g_destroyed.push_back(id); }
    int id;
};

}  // namespace

TEST_CASE("allocations bump through a chunk and keep their alignment") {
    PoolHandle parent = root().child("parent");
    Arena arena{*parent, "bump"};
    CHECK(arena.pool().parent() == parent->id());
    CHECK(arena.pool().path() == "root/parent/bump");
    CHECK(arena.chunk_bytes() == 0);

    void* a = arena.allocate(10);
    void* b = arena.allocate(10);
    CHECK(addr(b) == addr(a) + round_up(10));
    char* c = static_cast<char*>(arena.allocate(1, 1));
    char* d = static_cast<char*>(arena.allocate(1, 1));
    CHECK(d == c + 1);
    for (std::size_t align : {1, 2, 8, 16, 64, 256, 4096}) {
        CAPTURE(align);
        arena.allocate(3, 1);
        CHECK(addr(arena.allocate(24, align)) % align == 0);
    }
    CHECK(arena.chunk_bytes() == ARENA_FIRST_CHUNK);
    CHECK(parent->live() == ARENA_FIRST_CHUNK);
    CHECK(arena.allocate(0) != nullptr);
}

TEST_CASE("chunks grow from 64 KiB to 4 MiB and then stay at 4 MiB") {
    Arena arena{root(), "growth"};
    std::vector<std::size_t> sizes;
    std::size_t held = 0;
    while (sizes.size() < 9) {
        arena.allocate(16 * KIB);
        if (arena.chunk_bytes() != held) {
            sizes.push_back(arena.chunk_bytes() - held);
            held = arena.chunk_bytes();
            CHECK(arena.pool().live() == held);
            CHECK(arena.pool().blocks() == sizes.size());
        }
    }
    const std::vector<std::size_t> expected = {64 * KIB,  128 * KIB, 256 * KIB,
                                               512 * KIB, 1 * MIB,   2 * MIB,
                                               4 * MIB,   4 * MIB,   4 * MIB};
    CHECK(sizes == expected);
}

TEST_CASE("room and next_chunk tell what the next allocation takes") {
    Arena arena{root(), "room"};
    CHECK(arena.room() == 0);
    CHECK(arena.next_chunk() == ARENA_FIRST_CHUNK);
    arena.allocate(100, 1);
    CHECK(arena.room() == ARENA_FIRST_CHUNK - Arena::CHUNK_HEADER - 100);
    CHECK(arena.next_chunk() == 2 * ARENA_FIRST_CHUNK);
    arena.allocate(arena.room(), 1);
    CHECK(arena.room() == 0);
    CHECK(arena.chunk_bytes() == ARENA_FIRST_CHUNK);
    arena.allocate(1, 1);
    CHECK(arena.chunk_bytes() == 3 * ARENA_FIRST_CHUNK);
    while (arena.next_chunk() < ARENA_MAX_CHUNK)
        arena.allocate(arena.room() + 1, 1);
    arena.allocate(arena.room() + 1, 1);
    CHECK(arena.next_chunk() == ARENA_MAX_CHUNK);
}

TEST_CASE("a request bigger than the next chunk gets a chunk of its own") {
    Arena arena{root(), "oversized"};
    void* small = arena.allocate(100);
    std::size_t before = arena.chunk_bytes();
    void* big = arena.allocate(10 * MIB, 4096);
    CHECK(addr(big) % 4096 == 0);
    std::size_t own = arena.chunk_bytes() - before;
    CHECK(own >= 10 * MIB);
    CHECK(own < 10 * MIB + 4096 + 64);
    void* next = arena.allocate(100);
    CHECK(addr(next) == addr(small) + round_up(100));
    CHECK(arena.pool().live() >= arena.chunk_bytes());
    CHECK(arena.pool().blocks() == 2);
    arena.reset();
    CHECK(arena.chunk_bytes() == ARENA_FIRST_CHUNK);
    CHECK(arena.pool().live() == ARENA_FIRST_CHUNK);
}

TEST_CASE("reset keeps the first chunk and release frees every chunk") {
    PoolHandle parent = root().child("owner");
    Arena arena{*parent, "reset"};
    void* first = arena.allocate(64);
    for (int i = 0; i < 100; ++i) arena.allocate(64 * KIB);
    CHECK(arena.chunk_bytes() > ARENA_FIRST_CHUNK);
    CHECK(parent->live() == arena.chunk_bytes());

    arena.reset();
    CHECK(arena.chunk_bytes() == ARENA_FIRST_CHUNK);
    CHECK(parent->live() == ARENA_FIRST_CHUNK);
    CHECK(parent->blocks() == 1);
    CHECK(arena.allocate(64) == first);

    // Growth starts again after the kept chunk.
    arena.allocate(ARENA_FIRST_CHUNK);
    CHECK(arena.chunk_bytes() == ARENA_FIRST_CHUNK + 128 * KIB);

    arena.release();
    CHECK(arena.chunk_bytes() == 0);
    CHECK(parent->live() == 0);
    CHECK(parent->blocks() == 0);
    arena.allocate(8);
    CHECK(arena.chunk_bytes() == ARENA_FIRST_CHUNK);
}

TEST_CASE("the destructor list runs newest first, once per object") {
    g_destroyed.clear();
    {
        Arena arena{root(), "destroyed"};
        for (int i = 0; i < 5; ++i) {
            CHECK(arena.make_destroyed<Tracked>(i)->id == i);
        }
        int* plain = arena.make<int>(42);
        CHECK(*plain == 42);
        CHECK(g_destroyed.empty());
        arena.reset();
        CHECK(g_destroyed == std::vector<int>{4, 3, 2, 1, 0});

        arena.make_destroyed<Tracked>(10);
        arena.make_destroyed<Tracked>(11);
        arena.release();
        CHECK(g_destroyed == std::vector<int>{4, 3, 2, 1, 0, 11, 10});

        arena.make_destroyed<Tracked>(20);
    }
    CHECK(g_destroyed == std::vector<int>{4, 3, 2, 1, 0, 11, 10, 20});
}

struct Spawner {
    explicit Spawner(Arena& owner) : arena(&owner) {}
    ~Spawner() { arena->make_destroyed<Tracked>(99); }
    Arena* arena;
};

TEST_CASE("an object a destructor puts on the list is destroyed too") {
    g_destroyed.clear();
    Arena arena{root(), "spawn"};
    arena.make_destroyed<Tracked>(1);
    arena.make_destroyed<Spawner>(arena);
    arena.reset();
    CHECK(g_destroyed == std::vector<int>{1, 99});
}

TEST_CASE("a failed reserve keeps the standing reservation") {
    PoolHandle parent = root().child("limited", PoolKind::Scratch, {MIB});
    Arena arena{*parent, "reserve"};
    CHECK(arena.reserve(512 * KIB) == Status::Ok);
    CHECK(arena.reserve(4 * MIB) == Status::OverBudget);
    CHECK(arena.reserved() == 512 * KIB);
    CHECK(arena.pool().reserved() == 512 * KIB);
    CHECK(arena.reserve(SIZE_MAX) == Status::OverBudget);
    CHECK(arena.reserved() == 512 * KIB);
}

TEST_CASE("the destructor list survives growth past many chunks") {
    g_destroyed.clear();
    {
        Arena arena{root(), "many"};
        for (int i = 0; i < 2000; ++i) {
            arena.make_destroyed<Tracked>(i);
            arena.allocate(4 * KIB);
        }
    }
    REQUIRE(g_destroyed.size() == 2000);
    bool newest_first = true;
    for (int i = 0; i < 2000; ++i) {
        newest_first &= g_destroyed[static_cast<std::size_t>(i)] == 1999 - i;
    }
    CHECK(newest_first);
}

TEST_CASE("a vector with ArenaAllocator takes its storage from the arena") {
    Arena arena{root(), "vector"};
    Arena other{root(), "other"};
    {
        std::vector<int, ArenaAllocator<int>> v{ArenaAllocator<int>{arena}};
        for (int i = 0; i < 10000; ++i) v.push_back(i);
        bool intact = true;
        for (int i = 0; i < 10000; ++i) {
            intact &= v[static_cast<std::size_t>(i)] == i;
        }
        CHECK(intact);
        CHECK(arena.chunk_bytes() >= 10000 * sizeof(int));
        CHECK(other.chunk_bytes() == 0);
    }
    ArenaAllocator<int> a{arena};
    ArenaAllocator<double> rebound{a};
    CHECK(a == rebound);
    CHECK(&rebound.arena() == &arena);
    CHECK(a != ArenaAllocator<int>{other});
    std::size_t held = arena.chunk_bytes();
    arena.reset();
    CHECK(arena.chunk_bytes() <= held);
}

// A huge chunk is a new mapping on the page heap. A smaller one could come
// from a cached run, which is already mapped and passes the cap.
TEST_CASE("the hard cap makes try_allocate return null") {
    Arena arena{root(), "capped"};
    arena.allocate(64);
    {
        HardCap cap{1};
        CHECK(arena.try_allocate(80 * MIB) == nullptr);
        CHECK_THROWS_AS(arena.allocate(80 * MIB), std::bad_alloc);
        CHECK(arena.try_allocate(64) != nullptr);
    }
    CHECK(arena.try_allocate(80 * MIB) != nullptr);
}

TEST_CASE("reserve tops up the standing reservation") {
    PoolHandle parent = root().child("reserving");
    Arena arena{*parent, "reserve"};
    CHECK(arena.reserve(MIB) == Status::Ok);
    CHECK(arena.reserved() == MIB);
    CHECK(arena.pool().reserved() == MIB);

    for (int i = 0; i < 20; ++i) arena.allocate(64 * KIB);
    std::size_t held = arena.chunk_bytes();
    REQUIRE(held > MIB);
    CHECK(arena.reserved() == MIB);
    CHECK(arena.reserve(MIB) == Status::Ok);
    CHECK(arena.reserved() == held + MIB);
    CHECK(arena.reserve(0) == Status::Ok);
    CHECK(arena.reserved() == held + MIB);

    arena.release();
    CHECK(arena.reserved() == 0);
    CHECK(arena.pool().reserved() == 0);
    CHECK(arena.reserve(2 * MIB) == Status::Ok);
    CHECK(arena.reserved() == 2 * MIB);
}

TEST_CASE("an arena without a parent opens its pool below the scope pool") {
    PoolHandle parent = root().child("scope");
    Scope s{*parent};
    Arena arena{"inner"};
    CHECK(arena.pool().parent() == parent->id());
    arena.allocate(100);
    CHECK(parent->live() == ARENA_FIRST_CHUNK);
}

TEST_CASE("planted arena use after reset" * doctest::skip()) {
    Arena arena{root(), "planted"};
    auto* p = static_cast<volatile char*>(arena.allocate(64));
    p[0] = 1;
    arena.reset();
    p[0] = 2;
}
