#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

using namespace dftracer::utils;

namespace {

memory::Pool& root() { return memory::Manager::get().root(); }

int g_alive = 0;

// A callable of BYTES bytes that counts its live copies.
template <std::size_t BYTES>
struct Counted {
    Counted() { ++g_alive; }
    Counted(const Counted& other) : pad(other.pad) { ++g_alive; }
    Counted(Counted&& other) noexcept : pad(other.pad) { ++g_alive; }
    ~Counted() { --g_alive; }
    int operator()(int x) { return x + static_cast<int>(pad.size()); }
    std::array<char, BYTES> pad{};
};

using Small = Counted<16>;
using Large = Counted<64>;

struct ThrowingMove {
    ThrowingMove() = default;
    ThrowingMove(ThrowingMove&&) {}
    int operator()(int x) { return x; }
};

int twice(int x) { return 2 * x; }

}  // namespace

static_assert(sizeof(dstd::Function<void()>) == 32);
static_assert(dstd::Function<void()>::BUFFER_SIZE == 24);
static_assert(!std::is_copy_constructible_v<dstd::Function<void()>>);
static_assert(std::is_nothrow_move_constructible_v<dstd::Function<void()>>);

TEST_CASE("a small callable stays in the buffer") {
    memory::PoolHandle pool = root().child("small");
    memory::Scope s{*pool};
    {
        int base = 40;
        dstd::Function<int(int)> f = [base](int x) { return base + x; };
        CHECK(f(2) == 42);
        dstd::Function<int(int)> g = Small{};
        CHECK(g(1) == 17);
        dstd::Function<int(int)> p = &twice;
        CHECK(p(4) == 8);
        CHECK(pool->live() == 0);
    }
    CHECK(g_alive == 0);
    CHECK(pool->live() == 0);
}

TEST_CASE("a large callable goes to the heap and is freed") {
    memory::PoolHandle pool = root().child("large");
    memory::Scope s{*pool};
    {
        dstd::Function<int(int)> f = Large{};
        CHECK(f(1) == 65);
        CHECK(pool->blocks() == 1);
        CHECK(pool->live() >= sizeof(Large));
        dstd::Function<int(int)> g = std::move(f);
        CHECK(pool->blocks() == 1);
        CHECK(g(0) == 64);

        dstd::Function<int(int)> t = ThrowingMove{};
        CHECK(pool->blocks() == 2);
    }
    CHECK(g_alive == 0);
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("move leaves the source empty and destroys each target once") {
    {
        dstd::Function<int(int)> a = Small{};
        CHECK(g_alive == 1);
        dstd::Function<int(int)> b = std::move(a);
        CHECK_FALSE(a);
        CHECK(b);
        CHECK(g_alive == 1);
        b = Large{};
        CHECK(g_alive == 1);
        dstd::Function<int(int)> c = Small{};
        c = std::move(b);
        CHECK_FALSE(b);
        CHECK(g_alive == 1);
        c.swap(a);
        CHECK(a);
        CHECK_FALSE(c);
        CHECK(g_alive == 1);
        a = nullptr;
        CHECK_FALSE(a);
        CHECK(g_alive == 0);
        a = Small{};
        CHECK(g_alive == 1);
    }
    CHECK(g_alive == 0);
}

TEST_CASE("a move-only capture works") {
    memory::PoolHandle pool = root().child("move-only");
    memory::Scope s{*pool};
    {
        auto owned = std::make_unique<int>(7);
        dstd::Function<int()> f = [p = std::move(owned)] { return *p; };
        CHECK(f() == 7);
        dstd::Function<int()> g = std::move(f);
        CHECK(g() == 7);
    }
    CHECK(pool->live() == 0);
}

TEST_CASE("operator() is const and calls a mutable target") {
    const dstd::Function<int()> counter = [n = 0]() mutable { return ++n; };
    CHECK(counter() == 1);
    CHECK(counter() == 2);
}

TEST_CASE("an empty Function throws when called") {
    dstd::Function<void()> empty;
    CHECK_FALSE(empty);
    CHECK(empty == nullptr);
    CHECK_THROWS_AS(empty(), std::bad_function_call);
    int (*null_fn)(int) = nullptr;
    dstd::Function<int(int)> from_null = null_fn;
    CHECK_FALSE(from_null);
}

TEST_CASE("arguments and results pass through") {
    dstd::Function<std::unique_ptr<int>(std::unique_ptr<int>, int&)> f =
        [](std::unique_ptr<int> p, int& out) {
            out = *p;
            return p;
        };
    int out = 0;
    auto back = f(std::make_unique<int>(5), out);
    CHECK(out == 5);
    CHECK(*back == 5);

    dstd::Function<void(int&)> v = [](int& x) { x = 9; };
    v(out);
    CHECK(out == 9);
}

namespace {

struct ThrowingCopy {
    ThrowingCopy() = default;
    ThrowingCopy(const ThrowingCopy&) { throw std::runtime_error("copy"); }
    ThrowingCopy(ThrowingCopy&&) noexcept = default;
    int operator()(int x) { return x; }
    std::array<char, 64> pad{};
};

struct alignas(32) OverAligned {
    int operator()(int x) const {
        return reinterpret_cast<std::uintptr_t>(this) % 32 == 0 ? x : -1;
    }
};

}  // namespace

TEST_CASE("a throwing copy into the heap frees the block") {
    memory::PoolHandle pool = root().child("throwing");
    memory::Scope s{*pool};
    ThrowingCopy source;
    CHECK_THROWS_AS(dstd::Function<int(int)>{source}, std::runtime_error);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("an over-aligned callable goes to the heap with its alignment") {
    memory::PoolHandle pool = root().child("aligned");
    memory::Scope s{*pool};
    {
        dstd::Function<int(int)> f = OverAligned{};
        CHECK(pool->blocks() == 1);
        CHECK(f(3) == 3);
    }
    CHECK(pool->blocks() == 0);
}

TEST_CASE("swap and self-move keep every target exactly once") {
    {
        dstd::Function<int(int)> inline_one = Small{};
        dstd::Function<int(int)> heap_one = Large{};
        inline_one.swap(heap_one);
        CHECK(inline_one(0) == 64);
        CHECK(heap_one(0) == 16);
        CHECK(g_alive == 2);
        inline_one.swap(inline_one);
        CHECK(inline_one(0) == 64);
        dstd::Function<int(int)>& alias = heap_one;
        heap_one = std::move(alias);
        CHECK(heap_one(0) == 16);
        CHECK(g_alive == 2);
    }
    CHECK(g_alive == 0);
}
