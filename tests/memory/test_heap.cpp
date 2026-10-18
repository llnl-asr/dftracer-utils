#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/asan.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/heap_page.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <set>
#include <thread>
#include <vector>

#include "fresh_process.h"

using namespace dftracer::utils::memory;

namespace {

constexpr std::size_t KIB = 1024;
constexpr std::size_t MIB = 1024 * KIB;

std::size_t live() { return page_stats().live; }
std::size_t reserved() { return page_stats().reserved; }

std::size_t expected_class_size(std::size_t idx) {
    if (idx < 4) return (idx + 1) * 16;
    std::size_t octave = 64;
    std::size_t left = idx - 4;
    while (left >= 4) {
        octave *= 2;
        left -= 4;
    }
    return octave + (left + 1) * octave / 4;
}

std::size_t round_up(std::size_t n, std::size_t to) {
    return (n + to - 1) / to * to;
}

// Eight classes per doubling: a size in (2^k, 2^(k+1)] rounds up to a
// multiple of 2^k / 8.
std::size_t large_run(std::size_t size) {
    if (size <= MIB) return MIB;
    std::size_t k = MIB;
    while (2 * k < size) k *= 2;
    return round_up(size, k / 8);
}

std::size_t huge_bytes(std::size_t size) {
    return round_up(size + HEAP_PAGE_SIZE, HEAP_PAGE_SIZE);
}

void touch(void* p, std::size_t size) {
    if (size == 0) return;
    auto* b = static_cast<unsigned char*>(p);
    if (size <= MIB) {
        std::memset(b, 0xab, size);
    } else {
        b[0] = 0xab;
        b[size - 1] = 0xab;
    }
    CHECK(b[0] == 0xab);
    CHECK(b[size - 1] == 0xab);
}

int run_in_child(void (*body)()) {
    pid_t pid = ::fork();
    if (pid == 0) {
        int null = ::open("/dev/null", O_WRONLY);
        ::dup2(null, STDERR_FILENO);
        ::dup2(null, STDOUT_FILENO);
        ::signal(SIGABRT, SIG_DFL);
        body();
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return status;
}

bool aborted(int status) {
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

bool exited_cleanly(int status) {
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

TEST_CASE("live and reserved counters are exact in a fresh process") {
    CHECK(fresh_process::run("counters scenario").ok());
}

TEST_CASE("counters scenario" * doctest::skip()) {
    fresh_process::announce();
    std::vector<PageStats> seen;
    std::thread worker([&seen] {
        auto snap = [&seen] { seen.push_back(page_stats()); };
        snap();
        void* a = page_allocate(100, 16);
        snap();
        void* b = page_allocate(100, 16);
        snap();
        void* m = page_allocate(100000, 16);
        snap();
        void* l = page_allocate(2 * MIB, 16);
        snap();
        void* h = page_allocate(100 * MIB, 16);
        snap();
        page_deallocate(h, 100 * MIB);
        snap();
        page_deallocate(m, 100000);
        snap();
        page_deallocate(l, 2 * MIB);
        snap();
        page_deallocate(a, 100);
        page_deallocate(b);
        snap();
        std::vector<void*> runs;
        for (int i = 0; i < 4; ++i) runs.push_back(page_allocate(MIB, 16));
        snap();
        for (void* p : runs) page_deallocate(p, MIB);
        snap();
    });
    worker.join();

    REQUIRE(seen.size() == 12);
    auto expect = [&](std::size_t i, std::size_t l, std::size_t r) {
        CAPTURE(i);
        CHECK(seen[i].live - seen[0].live == l);
        CHECK(seen[i].reserved - seen[0].reserved == r);
    };
    const std::size_t span = HEAP_SPAN_SIZE;
    const std::size_t big = huge_bytes(100 * MIB);
    const std::size_t run = 2 * MIB;
    const std::size_t mapped = run + HEAP_PAGE_SIZE;
    const std::size_t heap = HEAP_PAGE_SIZE;
    const std::size_t small = 224 + 2 * HEAP_PAGE_SIZE;
    // Every growth here stays within the retention goal, so none purges.
    expect(1, 112, heap + span);
    expect(2, 224, heap + span);
    expect(3, small, heap + span);
    expect(4, small + run, heap + span + mapped);
    expect(5, small + run + 100 * MIB, heap + span + mapped + big);
    expect(6, small + run, heap + span + mapped);
    expect(7, 224 + run, heap + span + mapped);
    expect(8, 224, heap + span + mapped);
    expect(9, 0, heap + span + mapped);
    expect(10, 4 * MIB, heap + 2 * span + mapped);
    expect(11, 0, heap + 2 * span + mapped);
}

TEST_CASE("the class table has four classes per power of two") {
    REQUIRE(page_class_count() == 40);
    std::size_t worst_waste = 0;
    for (std::size_t i = 0; i < page_class_count(); ++i) {
        CAPTURE(i);
        std::size_t size = page_class_size(i);
        CHECK(size == expected_class_size(i));
        CHECK(size % 16 == 0);
        if (i > 0) CHECK(size > page_class_size(i - 1));
        std::size_t per_page = HEAP_PAGE_SIZE / size;
        CHECK(per_page >= 2);
        worst_waste = std::max(worst_waste, HEAP_PAGE_SIZE - per_page * size);
    }
    CHECK(page_class_size(0) == 16);
    CHECK(page_class_size(page_class_count() - 1) == SMALL_MAX);
    CHECK(worst_waste * 4 <= HEAP_PAGE_SIZE);
}

TEST_CASE("every small class boundary lands in its class") {
    std::size_t base = live();
    for (std::size_t i = 0; i < page_class_count(); ++i) {
        CAPTURE(i);
        std::size_t size = page_class_size(i);

        void* exact = page_allocate(size, 16);
        CHECK(live() - base == size);
        page_deallocate(exact, size);

        void* over = page_allocate(size + 1, 16);
        std::size_t over_block = i + 1 < page_class_count()
                                     ? page_class_size(i + 1)
                                     : HEAP_PAGE_SIZE;
        CHECK(live() - base == over_block);
        page_deallocate(over, size + 1);
        CHECK(live() == base);
    }
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{16}}) {
        void* p = page_allocate(size, 16);
        CHECK(live() - base == 16);
        page_deallocate(p, size);
    }
}

TEST_CASE("the tier boundaries pick small, medium, large and huge") {
    std::size_t base_live = live();

    void* small = page_allocate(SMALL_MAX, 16);
    CHECK(live() - base_live == SMALL_MAX);
    page_deallocate(small, SMALL_MAX);

    void* medium = page_allocate(SMALL_MAX + 1, 16);
    CHECK(live() - base_live == HEAP_PAGE_SIZE);
    page_deallocate(medium, SMALL_MAX + 1);

    void* medium_top = page_allocate(MEDIUM_MAX, 16);
    CHECK(live() - base_live == MEDIUM_MAX);
    page_deallocate(medium_top, MEDIUM_MAX);

    // A freed run is cached and still counted. Trim purges it and keeps
    // only its header page. Trimming first makes the bytes exact.
    for (std::size_t size : {MEDIUM_MAX + 1, LARGE_MAX}) {
        CAPTURE(size);
        std::size_t run = large_run(size);
        page_trim(Mode::All);
        std::size_t before = reserved();
        void* large = page_allocate(size, 16);
        std::size_t allocated = reserved();
        CHECK(live() - base_live == run);
        CHECK(allocated - before >= run);
        page_deallocate(large, size);
        CHECK(reserved() == allocated);
        page_trim(Mode::All);
        CHECK(allocated - reserved() == run);
        CHECK(reserved() - before <= HEAP_PAGE_SIZE);
    }
    CHECK(large_run(MEDIUM_MAX + 1) == MIB + 128 * KIB);
    CHECK(large_run(LARGE_MAX) == 64 * MIB);

    std::size_t before_huge = reserved();
    void* huge = page_allocate(LARGE_MAX + 1, 16);
    CHECK(live() - base_live == huge_bytes(LARGE_MAX + 1) - HEAP_PAGE_SIZE);
    CHECK(reserved() - before_huge == huge_bytes(LARGE_MAX + 1));
    page_deallocate(huge, LARGE_MAX + 1);

    CHECK(live() == base_live);
}

TEST_CASE("every alignment from 1 to 2 MiB at every size") {
    const std::vector<std::size_t> sizes = {0,
                                            1,
                                            15,
                                            16,
                                            17,
                                            64,
                                            65,
                                            100,
                                            4 * KIB,
                                            SMALL_MAX,
                                            SMALL_MAX + 1,
                                            64 * KIB,
                                            MEDIUM_MAX,
                                            MEDIUM_MAX + 1,
                                            5 * MIB,
                                            LARGE_MAX,
                                            LARGE_MAX + 1};
    std::size_t base = live();
    for (std::size_t align = 1; align <= MAX_ALIGN; align *= 2) {
        for (std::size_t size : sizes) {
            CAPTURE(align);
            CAPTURE(size);
            void* p = page_allocate(size, align);
            REQUIRE(p != nullptr);
            CHECK(reinterpret_cast<std::uintptr_t>(p) % align == 0);
            touch(p, size);
            if (size % 2 == 0) {
                page_deallocate(p, size);
            } else {
                page_deallocate(p);
            }
            CHECK(live() == base);
        }
    }
}

TEST_CASE("a small class is aligned to its size up to 64 bytes") {
    std::vector<void*> blocks;
    for (std::size_t align : {16, 32, 64}) {
        for (int i = 0; i < 200; ++i) {
            void* p = page_allocate(100, align);
            CHECK(reinterpret_cast<std::uintptr_t>(p) % align == 0);
            blocks.push_back(p);
        }
    }
    for (void* p : blocks) page_deallocate(p, 100);
}

TEST_CASE("a freed block is reused") {
    for (std::size_t size : {std::size_t{24}, std::size_t{4000}, SMALL_MAX,
                             std::size_t{200 * KIB}, MEDIUM_MAX}) {
        CAPTURE(size);
        void* a = page_allocate(size, 16);
        page_deallocate(a, size);
        void* b = page_allocate(size, 16);
        CHECK(a == b);
        page_deallocate(b, size);
    }
}

TEST_CASE("live blocks never overlap and survive mixed frees") {
    std::mt19937_64 rng(42);
    std::size_t base = live();
    struct Block {
        unsigned char* p;
        std::size_t size;
        unsigned char tag;
    };
    std::vector<Block> blocks;
    for (int i = 0; i < 4000; ++i) {
        std::size_t size = 1 + rng() % (rng() % 8 == 0 ? 300 * KIB : 3000);
        auto tag = static_cast<unsigned char>(i);
        auto* p = static_cast<unsigned char*>(page_allocate(size, 16));
        std::memset(p, tag, size);
        blocks.push_back({p, size, tag});
    }
    std::shuffle(blocks.begin(), blocks.end(), rng);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const Block& b = blocks[i];
        bool intact = true;
        for (std::size_t k = 0; k < b.size; ++k) intact &= b.p[k] == b.tag;
        CHECK(intact);
        page_deallocate(b.p, b.size);
        if (i % 3 == 0) {
            auto* q = static_cast<unsigned char*>(page_allocate(64, 16));
            std::memset(q, 0x5a, 64);
            page_deallocate(q, 64);
        }
    }
    CHECK(live() == base);
}

TEST_CASE("an emptied page serves another class") {
    std::size_t base = live();
    std::vector<void*> a;
    for (int i = 0; i < 1000; ++i) a.push_back(page_allocate(48, 16));
    for (void* p : a) page_deallocate(p, 48);
    std::vector<void*> b;
    for (int i = 0; i < 500; ++i) b.push_back(page_allocate(2000, 16));
    std::set<void*> distinct(b.begin(), b.end());
    CHECK(distinct.size() == b.size());
    for (void* p : b) page_deallocate(p, 2000);
    CHECK(live() == base);
}

TEST_CASE("a huge block grows and keeps its contents") {
    std::size_t base_live = live();
    std::size_t base_reserved = reserved();
    std::size_t old_size = 80 * MIB;
    std::size_t new_size = 160 * MIB;
    auto* p = static_cast<unsigned char*>(page_allocate(old_size, 64));
    for (std::size_t i = 0; i < old_size; i += 1 * MIB) p[i] = 0x11;
    p[old_size - 1] = 0x22;

    p = static_cast<unsigned char*>(page_grow_huge(p, new_size, 64));
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
    bool intact = p[old_size - 1] == 0x22;
    for (std::size_t i = 0; i < old_size; i += 1 * MIB) intact &= p[i] == 0x11;
    CHECK(intact);
    p[new_size - 1] = 0x33;
    CHECK(live() - base_live == huge_bytes(new_size) - HEAP_PAGE_SIZE);
    CHECK(reserved() - base_reserved == huge_bytes(new_size));

    page_deallocate(p, new_size);
    CHECK(live() == base_live);
    CHECK(reserved() == base_reserved);
}

TEST_CASE("a sized free with the wrong size aborts in a checked build") {
    struct Case {
        std::size_t size;
        std::size_t wrong;
    };
    static Case current;
    const std::vector<Case> cases = {{100, 5000},
                                     {5000, 100},
                                     {40 * KIB, 200 * KIB},
                                     {2 * MIB, 3 * MIB},
                                     {100 * MIB, 101 * MIB}};
    for (const Case& c : cases) {
        CAPTURE(c.size);
        CAPTURE(c.wrong);
        current = c;
        int status = run_in_child([] {
            void* p = page_allocate(current.size, 16);
            page_deallocate(p, current.wrong);
        });
        if (page_checks_enabled()) {
            CHECK(aborted(status));
        } else {
            CHECK(exited_cleanly(status));
        }
    }
}

TEST_CASE("a sized free with the right size and an unsized free do not abort") {
    int status = run_in_child([] {
        void* a = page_allocate(100, 64);
        page_deallocate(a, 100);
        void* b = page_allocate(5000, 16);
        page_deallocate(b);
        void* c = page_allocate(0, 16);
        page_deallocate(c, 0);
    });
    CHECK(exited_cleanly(status));
}

TEST_CASE("a block freed by another thread is accepted") {
    std::size_t base = live();
    for (std::size_t size :
         {std::size_t{64}, std::size_t{200 * KIB}, std::size_t{2 * MIB}}) {
        void* p = page_allocate(size, 16);
        std::thread([p, size] { page_deallocate(p, size); }).join();
        CHECK(live() == base);
    }
}

TEST_CASE("an alignment that is not served aborts") {
    CHECK(aborted(run_in_child([] { page_allocate(8, 3); })));
    CHECK(aborted(run_in_child([] { page_allocate(8, 0); })));
    CHECK(aborted(run_in_child([] { page_allocate(8, 2 * MAX_ALIGN); })));
}

TEST_CASE("null frees are no-ops") {
    page_deallocate(nullptr);
    page_deallocate(nullptr, 0);
}

// Run only by the ctest memory_asan_page_heap_overrun, which expects ASan to
// report it.
TEST_CASE("planted page heap overrun" * doctest::skip()) {
    REQUIRE(config().backend == Backend::PageHeap);
    constexpr std::size_t size = 24;
    auto* p = static_cast<volatile char*>(allocate(size, 16));
    p[size] = 1;
    deallocate(const_cast<char*>(p), size);
}

namespace {

constexpr std::size_t PLANTED_LEAK_BYTES = 777;

// Drops the only pointer in a frame that scrub_stack then overwrites, so
// Valgrind finds no pointer to the block.
__attribute__((noinline)) void plant_leak() {
    void* p = allocate(PLANTED_LEAK_BYTES, 16);
    std::memset(p, 1, PLANTED_LEAK_BYTES);
    asm volatile("" : : "r"(p) : "memory");
}

__attribute__((noinline)) void scrub_stack() {
    volatile char junk[16 * KIB];
    for (std::size_t i = 0; i < sizeof(junk); ++i) junk[i] = 0;
}

}  // namespace

// Run only by the ctest memory_valgrind_planted_leak, which runs Valgrind on it
// and expects the block to be reported as definitely lost.
TEST_CASE("planted page heap leak" * doctest::skip()) {
    REQUIRE(config().backend == Backend::PageHeap);
    plant_leak();
    scrub_stack();
}

#ifdef DFTRACER_UTILS_MEMORY_ASAN
namespace {
bool poisoned(const void* p) { return __asan_address_is_poisoned(p) != 0; }
}  // namespace

TEST_CASE("the page heap poisons slack, free blocks and purged pages") {
    constexpr std::size_t small = 100;
    auto* s = static_cast<char*>(page_allocate(small, 16));
    std::size_t cls = page_class_size(page_class_for(small, 16));
    std::memset(s, 1, small);
    CHECK_FALSE(poisoned(s + small - 1));
    CHECK(poisoned(s + small));
    CHECK(poisoned(s + cls - 1));
    page_deallocate(s, small);
    CHECK(poisoned(s));
    CHECK(poisoned(s + cls - 1));

    constexpr std::size_t medium = HEAP_PAGE_SIZE + 5000;
    auto* m = static_cast<char*>(page_allocate(medium, 16));
    std::memset(m, 1, medium);
    CHECK_FALSE(poisoned(m + medium - 1));
    CHECK(poisoned(m + medium));
    CHECK(poisoned(m + 2 * HEAP_PAGE_SIZE - 1));
    page_deallocate(m, medium);
    CHECK(poisoned(m));
    page_trim(Mode::All);
    CHECK(poisoned(m));
    CHECK(poisoned(m + HEAP_PAGE_SIZE));

    constexpr std::size_t large = 3 * MIB - 100 * KIB;
    auto* l = static_cast<char*>(page_allocate(large, 16));
    std::memset(l, 1, large);
    CHECK_FALSE(poisoned(l + large - 1));
    CHECK(poisoned(l + large));
    CHECK(poisoned(l + large_run(large) - 1));
    page_deallocate(l, large);
    CHECK(poisoned(l));
    page_trim(Mode::All);
    CHECK(poisoned(l + MIB));

    constexpr std::size_t huge = 70 * MIB + 13;
    auto* h = static_cast<char*>(page_allocate(huge, 16));
    h[huge - 1] = 1;
    CHECK_FALSE(poisoned(h + huge - 1));
    CHECK(poisoned(h + huge + 8));
    page_deallocate(h, huge);
}
#endif

TEST_CASE("a large block is charged at most an eighth above its size") {
    // Pool counters sum the heaps of the active backend; the page_heap run of
    // this file covers the case.
    if (config().backend != Backend::PageHeap) return;
    PoolHandle pool = Manager::get().root().child("large_classes");
    REQUIRE(pool);
    Scope scope{*pool};
    for (std::size_t size : {MIB + 1, 3 * MIB, 33 * MIB, 64 * MIB}) {
        CAPTURE(size);
        void* p = page_allocate(size, 16);
        CHECK(pool->live() >= size);
        CHECK(pool->live() <= round_up(size * 9 / 8, HEAP_PAGE_SIZE));
        page_deallocate(p, size);
        CHECK(pool->live() == 0);
    }
}

TEST_CASE("a cached large run is reused only for its own class") {
    void* a = page_allocate(3 * MIB, 16);
    page_deallocate(a, 3 * MIB);
    void* same = page_allocate(3 * MIB - 200 * KIB, 16);
    CHECK(same == a);
    page_deallocate(same, 3 * MIB - 200 * KIB);
    void* other = page_allocate(3 * MIB + 300 * KIB, 16);
    CHECK(other != a);
    page_deallocate(other, 3 * MIB + 300 * KIB);
}

TEST_CASE("reallocate across large classes keeps the bytes") {
    auto fill = [](void* p, std::size_t n) {
        auto* b = static_cast<unsigned char*>(p);
        for (std::size_t i = 0; i < n; i += 4093)
            b[i] = static_cast<unsigned char>(i * 31);
    };
    auto holds = [](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        bool ok = true;
        for (std::size_t i = 0; i < n; i += 4093)
            ok &= b[i] == static_cast<unsigned char>(i * 31);
        return ok;
    };
    void* p = page_allocate(3 * MIB, 16);
    fill(p, 3 * MIB);
    void* grown = page_reallocate(p, 3 * MIB, 5 * MIB, 16);
    CHECK(holds(grown, 3 * MIB));
    void* shrunk = page_reallocate(grown, 5 * MIB, 3 * MIB / 2, 16);
    CHECK(holds(shrunk, 3 * MIB / 2));
    void* same =
        page_reallocate(shrunk, 3 * MIB / 2, 3 * MIB / 2 - 10 * KIB, 16);
    CHECK(same == shrunk);
    CHECK(holds(same, 3 * MIB / 2 - 10 * KIB));
    page_deallocate(same, 3 * MIB / 2 - 10 * KIB);
}

namespace {

std::size_t run_mapped(std::size_t size) {
    return large_run(size) + HEAP_PAGE_SIZE;
}

// Allocates and frees 5 MiB runs until more than @p bytes are cached.
void make_warm_idle(std::size_t bytes) {
    std::vector<void*> runs;
    for (std::size_t got = 0; got <= bytes; got += run_mapped(5 * MIB))
        runs.push_back(page_allocate(5 * MIB, 16));
    for (void* p : runs) page_deallocate(p, 5 * MIB);
}

}  // namespace

TEST_CASE("a cached run is reused for its own class") {
    CHECK(fresh_process::run("warm reuse scenario").ok());
}

TEST_CASE("warm reuse scenario" * doctest::skip()) {
    fresh_process::announce();
    void* a = page_allocate(3 * MIB, 16);
    void* c = page_allocate(5 * MIB, 16);
    page_deallocate(a, 3 * MIB);
    page_deallocate(c, 5 * MIB);
    const std::size_t reserved_before = reserved();
    void* again = page_allocate(3 * MIB - 100 * KIB, 16);
    CHECK(again == a);
    CHECK(reserved() == reserved_before);
    page_deallocate(again, 3 * MIB - 100 * KIB);
    CHECK(reserved() == reserved_before);
}

TEST_CASE("a cold run of the own class is reused when nothing warm fits") {
    CHECK(fresh_process::run("cold reuse scenario").ok());
}

TEST_CASE("cold reuse scenario" * doctest::skip()) {
    fresh_process::announce();
    void* keep = page_allocate(2 * MIB, 16);
    void* a = page_allocate(3 * MIB, 16);
    page_deallocate(a, 3 * MIB);
    page_trim(Mode::All);
    // 5 MiB is more than two classes above 3 MiB, so it is no candidate.
    make_warm_idle(12 * MIB);
    // The cold run is backed again and nothing else is purged.
    const std::size_t reserved_before = reserved();
    void* again = page_allocate(3 * MIB, 16);
    CHECK(again == a);
    CHECK(reserved() == reserved_before + large_run(3 * MIB));
    page_deallocate(again, 3 * MIB);
    page_deallocate(keep, 2 * MIB);
}

TEST_CASE("a medium block takes backed free pages before purged ones") {
    CHECK(fresh_process::run("backed run first scenario").ok());
}

TEST_CASE("backed run first scenario" * doctest::skip()) {
    fresh_process::announce();
    constexpr std::size_t BLOCK = MIB / 2;
    void* hold = page_allocate(100, 16);
    void* purged = page_allocate(BLOCK, 16);
    void* backed = page_allocate(BLOCK, 16);
    void* tail = page_allocate(BLOCK, 16);
    REQUIRE(purged < backed);
    page_deallocate(purged, BLOCK);
    page_trim(Mode::All);
    page_deallocate(backed, BLOCK);
    const std::size_t before = reserved();
    void* again = page_allocate(BLOCK, 16);
    CHECK(again == backed);
    CHECK(reserved() == before);
    page_deallocate(again, BLOCK);
    page_deallocate(tail, BLOCK);
    page_deallocate(hold, 100);
}

TEST_CASE("a large block takes a warm larger run before a cold own run") {
    CHECK(fresh_process::run("warm larger run scenario").ok());
}

TEST_CASE("warm larger run scenario" * doctest::skip()) {
    fresh_process::announce();
    constexpr std::size_t OWN = 3 * MIB;
    constexpr std::size_t LARGER = 3 * MIB + 300 * KIB;
    PoolHandle pool = Manager::get().root().child("warm_larger");
    REQUIRE(pool);
    Scope scope{*pool};
    void* cold = page_allocate(OWN, 16);
    void* warm = page_allocate(LARGER, 16);
    page_deallocate(cold, OWN);
    page_trim(Mode::All);
    page_deallocate(warm, LARGER);
    const std::size_t before = reserved();
    constexpr std::size_t REQUEST = OWN - 100 * KIB;
    auto* p = static_cast<unsigned char*>(page_allocate(REQUEST, 16));
    CHECK(p == warm);
    CHECK(reserved() == before);
    // The run keeps its own size: the pool is charged its usable bytes. Pool
    // counters sum the active backend's heaps.
    const bool page_backend = config().backend == Backend::PageHeap;
    if (page_backend) CHECK(pool->live() == large_run(LARGER));
    for (std::size_t i = 0; i < REQUEST; i += 4096) p[i] = 0x5A;
    auto* q =
        static_cast<unsigned char*>(page_reallocate(p, REQUEST, 4 * MIB, 16));
    bool kept = true;
    for (std::size_t i = 0; i < REQUEST; i += 4096) kept &= q[i] == 0x5A;
    CHECK(kept);
    page_deallocate(q, 4 * MIB);
    if (page_backend) CHECK(pool->live() == 0);
}
