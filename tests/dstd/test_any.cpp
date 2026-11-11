// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/any.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>

#include <any>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <typeinfo>
#include <utility>

using namespace dftracer::utils;

namespace {

memory::Pool& root() { return memory::detail::Manager::get().root(); }

int g_alive = 0;

template <std::size_t BYTES>
struct Counted {
    Counted() { ++g_alive; }
    explicit Counted(char c) {
        pad.fill(c);
        ++g_alive;
    }
    Counted(const Counted& other) : pad(other.pad) { ++g_alive; }
    Counted(Counted&& other) noexcept : pad(other.pad) { ++g_alive; }
    Counted& operator=(const Counted&) = default;
    ~Counted() { --g_alive; }
    std::array<char, BYTES> pad{};
};

using Small = Counted<16>;
using Large = Counted<64>;

}  // namespace

static_assert(sizeof(dstd::Any) == 32);
static_assert(dstd::Any::BUFFER_SIZE == 24);

TEST_CASE("a small value stays in the buffer") {
    memory::PoolHandle pool = root().child("small");
    memory::detail::Scope s{*pool};
    {
        dstd::Any a = 42;
        dstd::Any b = Small{'s'};
        dstd::Any c = 3.5;
        CHECK(dstd::any_cast<int>(a) == 42);
        CHECK(dstd::any_cast<const Small&>(b).pad[0] == 's');
        CHECK(pool->live() == 0);
    }
    CHECK(g_alive == 0);
}

TEST_CASE("a large value goes to the heap and is freed") {
    memory::PoolHandle pool = root().child("large");
    memory::detail::Scope s{*pool};
    {
        dstd::Any a = Large{'l'};
        CHECK(pool->blocks() == 1);
        dstd::Any copy = a;
        CHECK(pool->blocks() == 2);
        CHECK(dstd::any_cast<Large&>(copy).pad[63] == 'l');
        dstd::Any moved = std::move(a);
        CHECK(pool->blocks() == 2);
        CHECK_FALSE(a.has_value());
    }
    CHECK(g_alive == 0);
    CHECK(pool->live() == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("copy and move keep the value and destroy each one once") {
    {
        dstd::Any a = Small{'a'};
        dstd::Any b = a;
        CHECK(g_alive == 2);
        dstd::any_cast<Small&>(b).pad[0] = 'b';
        CHECK(dstd::any_cast<const Small&>(a).pad[0] == 'a');

        dstd::Any c = std::move(b);
        CHECK_FALSE(b.has_value());
        CHECK(g_alive == 2);

        c = a;
        CHECK(g_alive == 2);
        c = Large{'x'};
        CHECK(g_alive == 2);
        a = std::move(c);
        CHECK(g_alive == 1);
        CHECK(dstd::any_cast<Large&>(a).pad[0] == 'x');
        a.swap(b);
        CHECK_FALSE(a.has_value());
        CHECK(b.has_value());
        b.reset();
        CHECK(g_alive == 0);
        b.emplace<Small>('e');
        CHECK(g_alive == 1);
    }
    CHECK(g_alive == 0);
}

TEST_CASE("any_cast returns the value or null for a pointer") {
    dstd::Any a = dstd::String("text");
    CHECK(a.type() == typeid(dstd::String));
    CHECK(dstd::any_cast<dstd::String>(a) == "text");
    CHECK(dstd::any_cast<const dstd::String&>(a) == "text");
    dstd::any_cast<dstd::String&>(a) += "!";
    CHECK(*dstd::any_cast<dstd::String>(&a) == "text!");
    const dstd::Any& ca = a;
    CHECK(*dstd::any_cast<dstd::String>(&ca) == "text!");

    CHECK(dstd::any_cast<int>(&a) == nullptr);

    dstd::String taken = dstd::any_cast<dstd::String>(std::move(a));
    CHECK(taken == "text!");

    dstd::Any empty;
    CHECK_FALSE(empty.has_value());
    CHECK(empty.type() == typeid(void));
    CHECK(dstd::any_cast<int>(&empty) == nullptr);
    dstd::Any* none = nullptr;
    CHECK(dstd::any_cast<int>(none) == nullptr);
}

TEST_CASE("any_cast to another type ends the process" * doctest::skip()) {
    fresh_process::announce_abort();
    dstd::Any a = dstd::String("text");
    dstd::any_cast<int>(a);
}

TEST_CASE("any_cast of an empty Any ends the process" * doctest::skip()) {
    fresh_process::announce_abort();
    dstd::Any empty;
    dstd::any_cast<const int&>(empty);
}

TEST_CASE("a failed any_cast aborts with its message") {
    for (const char* test_case :
         {"any_cast to another type ends the process",
          "any_cast of an empty Any ends the process"}) {
        auto result = fresh_process::run(test_case);
        CHECK(result.aborted_with(
            "any_cast: the Any is empty or holds another type"));
    }
}

namespace {

int g_copies_left = 0;

struct FailingCopy {
    FailingCopy() { ++g_alive; }
    FailingCopy(const FailingCopy& other) : pad(other.pad) {
        if (g_copies_left-- == 0) throw std::runtime_error("copy");
        ++g_alive;
    }
    FailingCopy(FailingCopy&& other) noexcept : pad(other.pad) { ++g_alive; }
    ~FailingCopy() { --g_alive; }
    std::array<char, 64> pad{};
};

struct alignas(32) OverAligned {
    int value = 5;
};

}  // namespace

TEST_CASE("a throwing copy leaves no block and keeps the target") {
    memory::PoolHandle pool = root().child("throwing");
    memory::detail::Scope s{*pool};
    {
        dstd::Any a;
        a.emplace<FailingCopy>();
        CHECK(pool->blocks() == 1);
        g_copies_left = 0;
        CHECK_THROWS_AS(dstd::Any{a}, std::runtime_error);
        CHECK(pool->blocks() == 1);

        dstd::Any b = 1;
        g_copies_left = 0;
        CHECK_THROWS_AS(b = a, std::runtime_error);
        CHECK(dstd::any_cast<int>(b) == 1);
        CHECK(pool->blocks() == 1);

        FailingCopy source;
        g_copies_left = 0;
        CHECK_THROWS_AS(b.emplace<FailingCopy>(source), std::runtime_error);
        CHECK_FALSE(b.has_value());
        CHECK(pool->blocks() == 1);
        CHECK(g_alive == 2);
    }
    CHECK(g_alive == 0);
    CHECK(pool->blocks() == 0);
}

TEST_CASE("an over-aligned value goes to the heap with its alignment") {
    memory::PoolHandle pool = root().child("aligned");
    memory::detail::Scope s{*pool};
    {
        dstd::Any a = OverAligned{};
        CHECK(pool->blocks() == 1);
        const OverAligned* p = dstd::any_cast<OverAligned>(&a);
        REQUIRE(p != nullptr);
        CHECK(reinterpret_cast<std::uintptr_t>(p) % 32 == 0);
        CHECK(p->value == 5);
    }
    CHECK(pool->blocks() == 0);
}

TEST_CASE("swap, self-assignment and a non-copyable cast") {
    {
        dstd::Any small = Small{'s'};
        dstd::Any large = Large{'l'};
        small.swap(large);
        CHECK(dstd::any_cast<Large&>(small).pad[0] == 'l');
        CHECK(dstd::any_cast<Small&>(large).pad[0] == 's');
        CHECK(g_alive == 2);
        const dstd::Any& alias = small;
        small = alias;
        CHECK(dstd::any_cast<Large&>(small).pad[0] == 'l');
        small.swap(small);
        CHECK(small.has_value());
        CHECK(g_alive == 2);
    }
    CHECK(g_alive == 0);
    dstd::Any a = 1;
    CHECK(dstd::any_cast<dstd::UniquePtr<int>>(&a) == nullptr);
}
