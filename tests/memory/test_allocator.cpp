#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/allocator.h>
#include <dftracer/utils/memory/asan.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/heap_page.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace dftracer::utils::memory;

namespace {

constexpr std::size_t MIB = std::size_t{1} << 20;

Pool& root() { return Manager::get().root(); }

bool page_heap() { return config().backend == Backend::PageHeap; }

bool aligned(const void* p, std::size_t align) {
    return reinterpret_cast<std::uintptr_t>(p) % align == 0;
}

unsigned char pattern(std::size_t i) {
    return static_cast<unsigned char>((i * 131) ^ (i >> 12));
}

// Every byte up to 1 MiB, then one byte in each 4 KiB, so the huge cases stay
// fast in a debug build. The probes below a size do not depend on the size,
// so a block filled at one size can be checked at a smaller one.
template <class Fn>
void for_each_probe(std::size_t size, Fn&& fn) {
    std::size_t dense = size < MIB ? size : MIB;
    for (std::size_t i = 0; i < dense; ++i) fn(i);
    for (std::size_t i = dense; i < size; i += 4096) fn(i);
}

void fill(void* p, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(p);
    for_each_probe(size, [&](std::size_t i) { bytes[i] = pattern(i); });
}

bool holds(const void* p, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(p);
    bool ok = true;
    for_each_probe(size, [&](std::size_t i) { ok &= bytes[i] == pattern(i); });
    return ok;
}

// The child leaves with _exit, so the exit check does not run there.
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

struct alignas(64) Line {
    unsigned char bytes[64];
};

struct alignas(4096) Page {
    unsigned char bytes[4096];
};

}  // namespace

static_assert(std::is_empty_v<Allocator<int>>);
static_assert(std::allocator_traits<Allocator<int>>::is_always_equal::value);
static_assert(
    std::is_same_v<std::allocator_traits<Allocator<int>>::rebind_alloc<double>,
                   Allocator<double>>);
static_assert(std::is_same_v<
              std::allocator_traits<PoolAllocator<int>>::rebind_alloc<double>,
              PoolAllocator<double>>);
static_assert(
    !std::allocator_traits<PoolAllocator<int>>::is_always_equal::value);
static_assert(
    std::allocator_traits<
        PoolAllocator<int>>::propagate_on_container_copy_assignment::value);
static_assert(
    std::allocator_traits<
        PoolAllocator<int>>::propagate_on_container_move_assignment::value);
static_assert(std::allocator_traits<
              PoolAllocator<int>>::propagate_on_container_swap::value);
static_assert(!std::is_default_constructible_v<PoolAllocator<int>>);

TEST_CASE("a vector with Allocator charges the scope pool and frees to zero") {
    PoolHandle pool = root().child("vector");
    REQUIRE(pool);
    {
        Scope s{*pool};
        std::vector<std::uint64_t, Allocator<std::uint64_t>> v;
        for (std::uint64_t i = 0; i < 10000; ++i) v.push_back(i);
        CHECK(pool->live() >= 10000 * sizeof(std::uint64_t));
        CHECK(pool->blocks() == 1);
        v.clear();
        v.shrink_to_fit();
        CHECK(pool->live() == 0);
        CHECK(pool->blocks() == 0);
    }
    CHECK(pool->live() == 0);
}

TEST_CASE("Allocator blocks follow the scope that allocated them") {
    PoolHandle first = root().child("first");
    PoolHandle second = root().child("second");
    std::vector<int, Allocator<int>> v;
    {
        Scope s{*first};
        v.reserve(1000);
    }
    CHECK(first->live() >= 1000 * sizeof(int));
    {
        Scope s{*second};
        v.reserve(100000);
        CHECK(first->live() == 0);
        CHECK(second->live() >= 100000 * sizeof(int));
    }
    std::vector<int, Allocator<int>>().swap(v);
    CHECK(second->live() == 0);
}

TEST_CASE(
    "a container with PoolAllocator charges its pool whatever the scope") {
    PoolHandle named = root().child("named");
    PoolHandle scope = root().child("scope");
    REQUIRE(named);
    REQUIRE(scope);
    {
        Scope s{*scope};
        std::vector<std::uint32_t, PoolAllocator<std::uint32_t>> v{
            PoolAllocator<std::uint32_t>{*named}};
        for (std::uint32_t i = 0; i < 50000; ++i) v.push_back(i);
        std::list<int, PoolAllocator<int>> nodes{PoolAllocator<int>{*named}};
        for (int i = 0; i < 100; ++i) nodes.push_back(i);
        CHECK(named->live() >= 50000 * sizeof(std::uint32_t));
        CHECK(named->blocks() == 101);
        CHECK(scope->live() == 0);
        CHECK(scope->blocks() == 0);
    }
    CHECK(named->live() == 0);
    CHECK(named->blocks() == 0);
    CHECK(scope->live() == 0);
}

TEST_CASE("the pool overload of allocate charges the pool at every tier") {
    PoolHandle named = root().child("tiers");
    PoolHandle scope = root().child("elsewhere");
    const std::size_t sizes[] = {1, 100, 32768, 40000, MIB + 1, 70 * MIB};
    Scope s{*scope};
    for (std::size_t size : sizes) {
        CAPTURE(size);
        void* p = allocate(size, 16, *named);
        CHECK(named->blocks() == 1);
        CHECK(named->live() >= size);
        CHECK(scope->blocks() == 0);
        void* q = allocate(64, 16);
        CHECK(scope->blocks() == 1);
        deallocate(q, 64);
        deallocate(p, size);
        CHECK(named->live() == 0);
        CHECK(named->blocks() == 0);
    }
}

TEST_CASE("allocators compare equal by pool and propagate with the container") {
    CHECK(Allocator<int>{} == Allocator<double>{});

    PoolHandle a = root().child("a");
    PoolHandle b = root().child("b");
    PoolAllocator<int> on_a{*a};
    PoolAllocator<int> on_b{*b};
    PoolAllocator<double> rebound{on_a};
    CHECK(on_a == rebound);
    CHECK(&rebound.pool() == a.get());
    CHECK(on_a != on_b);

    using Vec = std::vector<int, PoolAllocator<int>>;
    {
        Vec x(100, 1, on_a);
        Vec y(10, 2, on_b);
        y = x;
        CHECK(y.get_allocator() == on_a);
        CHECK(b->live() == 0);

        Vec z(10, 3, on_b);
        z = std::move(x);
        CHECK(z.get_allocator() == on_a);

        Vec w(10, 4, on_b);
        w.swap(z);
        CHECK(w.get_allocator() == on_a);
        CHECK(z.get_allocator() == on_b);
        CHECK(w.size() == 100);
        CHECK(z.size() == 10);
    }
    CHECK(a->live() == 0);
    CHECK(b->live() == 0);
}

TEST_CASE("over-aligned types keep their alignment") {
    PoolHandle pool = root().child("aligned");
    std::vector<Line, Allocator<Line>> lines(3);
    std::vector<Page, Allocator<Page>> pages(3);
    std::vector<Line, PoolAllocator<Line>> pool_lines(
        5, PoolAllocator<Line>{*pool});
    std::vector<Page, PoolAllocator<Page>> pool_pages(
        5, PoolAllocator<Page>{*pool});
    CHECK(aligned(lines.data(), alignof(Line)));
    CHECK(aligned(pages.data(), alignof(Page)));
    CHECK(aligned(pool_lines.data(), alignof(Line)));
    CHECK(aligned(pool_pages.data(), alignof(Page)));
    CHECK(pool->blocks() == 2);
}

TEST_CASE("reallocate keeps the contents as it grows and shrinks") {
    PoolHandle pool = root().child("resize");
    const std::size_t sizes[] = {1,       100,      4096,     32768,
                                 40000,   200000,   MIB,      MIB + 1,
                                 3 * MIB, 70 * MIB, 150 * MIB};
    Scope s{*pool};
    void* p = nullptr;
    std::size_t size = 0;
    auto step = [&](std::size_t next) {
        CAPTURE(size);
        CAPTURE(next);
        p = reallocate(p, size, next, 64);
        CHECK(aligned(p, 64));
        CHECK(holds(p, size < next ? size : next));
        fill(p, next);
        size = next;
        CHECK(pool->blocks() == 1);
        CHECK(pool->live() >= size);
    };
    for (std::size_t next : sizes) step(next);
    for (auto it = std::rbegin(sizes) + 1; it != std::rend(sizes); ++it) {
        step(*it);
    }
    step(0);
    deallocate(p, 0);
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("reallocate keeps the pool of the old block") {
    PoolHandle owner = root().child("owner");
    PoolHandle other = root().child("other");
    const std::size_t sizes[] = {100, 40000, 3 * MIB, 70 * MIB};
    for (std::size_t from : sizes) {
        for (std::size_t to : sizes) {
            CAPTURE(from);
            CAPTURE(to);
            void* p;
            {
                Scope s{*owner};
                p = allocate(from, 16);
            }
            fill(p, from);
            {
                Scope s{*other};
                p = reallocate(p, from, to, 16);
            }
            CHECK(holds(p, from < to ? from : to));
            CHECK(owner->blocks() == 1);
            CHECK(owner->live() >= to);
            CHECK(other->blocks() == 0);
            deallocate(p, to);
            CHECK(owner->live() == 0);
        }
    }
}

TEST_CASE("reallocate stays in place when the block already fits") {
    struct Case {
        std::size_t from;
        std::size_t to;
    };
    // Each pair shares a size class or a run on the page heap.
    const Case cases[] = {{100, 110},
                          {110, 100},
                          {40000, 60000},
                          {60000, 40000},
                          {3 * MIB + 4096, 3 * MIB + 8192},
                          {70 * MIB + 100, 70 * MIB + 200},
                          {70 * MIB + 200, 70 * MIB + 100}};
    for (const Case& c : cases) {
        CAPTURE(c.from);
        CAPTURE(c.to);
        void* p = allocate(c.from, 16);
        fill(p, c.from);
        void* q = reallocate(p, c.from, c.to, 16);
        CHECK(holds(q, c.from < c.to ? c.from : c.to));
        if (page_heap()) CHECK(q == p);
        void* same = reallocate(q, c.to, c.to, 16);
        CHECK(same == q);
        deallocate(same, c.to);
    }
}

TEST_CASE("reallocate moves a block to a stricter alignment") {
    void* p = allocate(100, 16);
    fill(p, 100);
    while (aligned(p, 4096)) {
        void* next = allocate(100, 16);
        deallocate(p, 100);
        p = next;
        fill(p, 100);
    }
    p = reallocate(p, 100, 100, 4096);
    CHECK(aligned(p, 4096));
    CHECK(holds(p, 100));
    deallocate(p, 100);
}

TEST_CASE("reallocate of null allocates") {
    PoolHandle pool = root().child("null");
    Scope s{*pool};
    void* p = reallocate(nullptr, 0, 1000, 32);
    CHECK(aligned(p, 32));
    CHECK(pool->blocks() == 1);
    deallocate(p, 1000);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("reallocate with the wrong old size aborts in a checked build") {
    struct Case {
        std::size_t size;
        std::size_t wrong;
    };
    static Case current;
    const Case cases[] = {
        {100, 5000}, {40000, 200000}, {3 * MIB, 5 * MIB}, {70 * MIB, 71 * MIB}};
    for (const Case& c : cases) {
        CAPTURE(c.size);
        current = c;
        int status = run_in_child([] {
            void* p = allocate(current.size, 16);
            p = reallocate(p, current.wrong, current.size + 1, 16);
            deallocate(p, current.size + 1);
        });
        if (page_checks_enabled()) {
            CHECK(WIFSIGNALED(status));
            CHECK(WTERMSIG(status) == SIGABRT);
        } else {
            CHECK(WIFEXITED(status));
        }
    }
}

TEST_CASE("reallocate accepts a block another thread allocated") {
    PoolHandle pool = root().child("remote");
    const std::size_t sizes[][2] = {{100, 110},
                                    {100, 5000},
                                    {40000, 60000},
                                    {40000, 3 * MIB},
                                    {3 * MIB + 4096, 3 * MIB + 8192},
                                    {70 * MIB + 100, 70 * MIB + 200}};
    for (const auto& pair : sizes) {
        CAPTURE(pair[0]);
        CAPTURE(pair[1]);
        void* p = nullptr;
        std::thread([&] {
            Scope s{*pool};
            p = allocate(pair[0], 16);
            fill(p, pair[0]);
        }).join();
        p = reallocate(p, pair[0], pair[1], 16);
        CHECK(holds(p, pair[0]));
        CHECK(pool->blocks() == 1);
        deallocate(p, pair[1]);
        CHECK(pool->live() == 0);
        CHECK(pool->blocks() == 0);
    }
}

#ifdef DFTRACER_UTILS_MEMORY_ASAN
TEST_CASE("a block resized in place is poisoned past its new size") {
    if (!page_heap()) return;
    auto poisoned = [](const void* p) {
        return __asan_address_is_poisoned(p) != 0;
    };
    const std::size_t sizes[][2] = {{100, 110},
                                    {40000, 60000},
                                    {3 * MIB + 4096, 3 * MIB + 8192},
                                    {70 * MIB + 100, 70 * MIB + 200}};
    for (const auto& pair : sizes) {
        std::size_t small = pair[0];
        std::size_t big = pair[1];
        CAPTURE(small);
        auto* p = static_cast<char*>(allocate(small, 16));
        CHECK(poisoned(p + small));
        REQUIRE(reallocate(p, small, big, 16) == p);
        CHECK_FALSE(poisoned(p + big - 1));
        CHECK(poisoned(p + big));
        REQUIRE(reallocate(p, big, small, 16) == p);
        CHECK_FALSE(poisoned(p + small - 1));
        CHECK(poisoned(p + small));
        deallocate(p, small);
    }
}
#endif
