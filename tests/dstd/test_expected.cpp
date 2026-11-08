// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>

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

namespace {

dstd::Expected<int, dstd::Error> parse_positive(int input) {
    if (input <= 0)
        return dstd::Error{dstd::Condition::InvalidArgument, "not positive"};
    return input;
}

dstd::Expected<int, dstd::Error> doubled(int input) {
    auto parsed = parse_positive(input);
    if (!parsed) return parsed.error();
    return *parsed * 2;
}

}  // namespace

TEST_CASE("Expected passes a bare error on") {
    const auto good = doubled(4);
    REQUIRE(good.has_value());
    CHECK(*good == 8);
    const auto bad = doubled(-1);
    REQUIRE(!bad.has_value());
    CHECK(bad.error().is(dstd::Condition::InvalidArgument));
    CHECK(bad.error().message() == "not positive");
}

TEST_CASE("Expected match calls the handler of the side it holds") {
    const auto on_value = [](int value) { return value + 1; };
    const auto on_error = [](const dstd::Error&) { return -1; };
    CHECK(parse_positive(3).match(on_value, on_error) == 4);
    CHECK(parse_positive(0).match(on_value, on_error) == -1);

    dstd::Expected<void, int> done;
    dstd::Expected<void, int> failed = dstd::Unexpected(5);
    const auto ok = [] { return 0; };
    const auto code = [](int error) { return error; };
    CHECK(done.match(ok, code) == 0);
    CHECK(failed.match(ok, code) == 5);
}

TEST_CASE("Expected holds a move-only value") {
    dstd::Expected<dstd::UniquePtr<int>, int> held(dstd::make_unique<int>(7));
    auto moved = std::move(held);
    REQUIRE(moved.has_value());
    CHECK(**moved == 7);
}

TEST_CASE("Expected fits its layout") {
    CHECK(sizeof(dstd::Expected<void, dstd::Error>) == 24);
    CHECK(sizeof(dstd::Expected<int, int>) == 8);
}

// Run in a new process by the case below; each ends the process.
TEST_CASE("value() on an error ends the process" * doctest::skip()) {
    fresh_process::announce();
    auto bad = parse_positive(0);
    CHECK(bad.value() == 0);
}

TEST_CASE("an error result never checked ends the process" * doctest::skip()) {
    fresh_process::announce();
    auto bad = parse_positive(0);
}

TEST_CASE("a discarded error result does not end the process" *
          doctest::skip()) {
    fresh_process::announce();
    parse_positive(0).discard();
}

namespace {

bool printed(const fresh_process::Result& run, const char* text) {
    const dstd::StringView output(run.output.data(), run.output.size());
    return output.find(text) != dstd::StringView::npos;
}

}  // namespace

TEST_CASE("Expected ends the process on misuse") {
    const auto read_error =
        fresh_process::run("value() on an error ends the process");
    CHECK(!read_error.ok());
    CHECK(printed(read_error, "value() on an error result: "));
    CHECK(printed(read_error, "not positive"));

    const auto discarded =
        fresh_process::run("a discarded error result does not end the process");
    CHECK(discarded.ok());

#ifdef DFTRACER_UTILS_EXPECTED_CHECKS
    const auto unchecked =
        fresh_process::run("an error result never checked ends the process");
    CHECK(!unchecked.ok());
    CHECK(printed(unchecked, "an error result was never checked"));
#endif
}
