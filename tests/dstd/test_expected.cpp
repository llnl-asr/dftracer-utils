// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <doctest/doctest.h>

#include <utility>

using namespace dftracer::utils;

namespace {

dstd::Expected<dstd::String, int> make_result(int code) {
    if (code != 0) return dstd::Unexpected(code);
    return dstd::String("ok");
}

}  // namespace

TEST_CASE("Expected holds a value") {
    dstd::Expected<int, int> result(42);
    CHECK(result.has_value());
    CHECK(result.value() == 42);
    CHECK(result.value_or(0) == 42);
}

TEST_CASE("Expected holds an error") {
    dstd::Expected<int, int> result = dstd::Unexpected(7);
    CHECK(!result.has_value());
    CHECK(result.error() == 7);
    CHECK(result.value_or(0) == 0);
}

TEST_CASE("Expected void holds an error") {
    dstd::Expected<void, int> result = dstd::Unexpected(9);
    CHECK(!result.has_value());
    CHECK(result.error() == 9);
}

TEST_CASE("Unexpected deduces from a return expression") {
    dstd::Expected<dstd::String, int> result = make_result(0);
    REQUIRE(result.has_value());
    CHECK(result.value() == "ok");

    result = make_result(7);
    REQUIRE(!result.has_value());
    CHECK(result.error() == 7);
}

TEST_CASE("Expected transforms a value") {
    dstd::Expected<int, int> result(2);
    auto mapped =
        std::move(result).transform([](int value) { return value * 3; });
    REQUIRE(mapped.has_value());
    CHECK(mapped.value() == 6);
}

TEST_CASE("Expected chains a value") {
    dstd::Expected<int, int> result(2);
    auto bound = std::move(result).and_then([](int value) {
        return dstd::Expected<dstd::String, int>(dstd::to_string(value));
    });
    REQUIRE(bound.has_value());
    CHECK(bound.value() == "2");
}
