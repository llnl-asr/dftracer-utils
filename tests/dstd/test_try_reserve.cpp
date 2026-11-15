// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/try_reserve.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <limits>

using namespace dftracer::utils;

TEST_CASE("try_reserve grows a vector and a string") {
    dstd::Vector<std::uint64_t> values;
    REQUIRE(dstd::try_reserve(values, 1000).has_value());
    CHECK(values.capacity() >= 1000);

    dstd::String text;
    REQUIRE(dstd::try_reserve(text, 500).has_value());
    CHECK(text.capacity() >= 500);
}

TEST_CASE("try_reserve reports OutOfMemory and leaves the vector alone") {
    dstd::Vector<std::uint64_t> values{1, 2, 3};
    const std::size_t capacity = values.capacity();
    const auto result =
        dstd::try_reserve(values, std::numeric_limits<std::size_t>::max() / 16);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().condition() == dstd::Condition::OutOfMemory);
    CHECK_FALSE(result.error().message().empty());
    CHECK(values.capacity() == capacity);
    CHECK(values.size() == 3);

    const auto overflow =
        dstd::try_reserve(values, std::numeric_limits<std::size_t>::max());
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().condition() == dstd::Condition::OutOfMemory);
}

TEST_CASE("try_reserve reports OutOfMemory for a string") {
    dstd::String text = "abc";
    const auto result =
        dstd::try_reserve(text, std::numeric_limits<std::size_t>::max() / 4);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().condition() == dstd::Condition::OutOfMemory);
    CHECK(text == "abc");
}
