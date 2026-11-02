// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/function_ref.h>
#include <doctest/doctest.h>

#include <type_traits>

using namespace dftracer::utils;

namespace {

int twice(int x) { return 2 * x; }
int thrice(int x) { return 3 * x; }

int apply(dstd::FunctionRef<int(int)> fn, int x) { return fn(x); }

struct Counter {
    int calls = 0;
    int operator()(int x) {
        ++calls;
        return x;
    }
};

struct ConstOnly {
    int operator()(int x) const { return x + 1; }
};

struct MutableOnly {
    int operator()(int x) { return x; }
};

}  // namespace

static_assert(std::is_trivially_copyable_v<dstd::FunctionRef<int(int)>>);
static_assert(
    !std::is_constructible_v<dstd::FunctionRef<int(int)>, const MutableOnly&>,
    "a const object whose call operator is non-const is refused");

TEST_CASE("FunctionRef calls a lambda") {
    int base = 10;
    CHECK(apply([&](int x) { return base + x; }, 5) == 15);
}

TEST_CASE("FunctionRef calls a plain function and function pointers") {
    CHECK(apply(twice, 4) == 8);
    CHECK(apply(&twice, 4) == 8);
    int (*ptr)(int) = &thrice;
    dstd::FunctionRef<int(int)> held = ptr;
    ptr = &twice;
    CHECK(held(4) == 12);
}

TEST_CASE("FunctionRef refers to the object, not a copy") {
    Counter counter;
    dstd::FunctionRef<int(int)> ref = counter;
    ref(1);
    ref(2);
    CHECK(counter.calls == 2);
}

TEST_CASE("FunctionRef calls a const callable through const") {
    const ConstOnly fn;
    CHECK(apply(fn, 1) == 2);
}
