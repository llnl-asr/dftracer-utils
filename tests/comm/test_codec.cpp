// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/comm/codec.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>

using namespace dftracer::utils;

static_assert(comm::Codec<comm::StringCodec, dstd::String>);
static_assert(
    comm::Codec<comm::VectorCodec<std::uint64_t>, dstd::Vector<std::uint64_t>>);

namespace {

template <class C, class T>
dstd::Result<T> round_trip(const C& codec, const T& value) {
    dstd::Vector<std::byte> bytes(codec.serialized_size(value));
    auto written = codec.serialize(value, dstd::Span<std::byte>(bytes));
    REQUIRE(written.has_value());
    CHECK(*written == bytes.size());
    return codec.deserialize(dstd::Span<const std::byte>(bytes));
}

}  // namespace

TEST_SUITE("CommCodec") {
    TEST_CASE("a string crosses with its bytes, embedded NULs and empty too") {
        const comm::StringCodec codec;
        const dstd::String text("a\0b;c", 5);
        auto back = round_trip(codec, text);
        REQUIRE(back.has_value());
        CHECK(*back == text);
        auto empty = round_trip(codec, dstd::String());
        REQUIRE(empty.has_value());
        CHECK(empty->empty());
    }

    TEST_CASE("a string refuses an output of the wrong size") {
        const comm::StringCodec codec;
        const dstd::String text("abcd");
        dstd::Vector<std::byte> bytes(3);
        auto written = codec.serialize(text, dstd::Span<std::byte>(bytes));
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().is(dstd::Condition::InvalidArgument));
    }

    TEST_CASE("a vector of values crosses whole, empty too") {
        const comm::VectorCodec<std::uint64_t> codec;
        const dstd::Vector<std::uint64_t> values = {1, 0xFFFFFFFFFFFFFFFFull,
                                                    42};
        auto back = round_trip(codec, values);
        REQUIRE(back.has_value());
        CHECK(*back == values);
        auto empty = round_trip(codec, dstd::Vector<std::uint64_t>());
        REQUIRE(empty.has_value());
        CHECK(empty->empty());
    }

    TEST_CASE("a vector refuses bytes that are not a whole number of values") {
        const comm::VectorCodec<std::uint32_t> codec;
        dstd::Vector<std::byte> bytes(7);
        auto back = codec.deserialize(dstd::Span<const std::byte>(bytes));
        REQUIRE_FALSE(back.has_value());
        CHECK(back.error().is(dstd::Condition::InvalidArgument));
    }

    TEST_CASE("a vector refuses an output of the wrong size") {
        const comm::VectorCodec<std::uint32_t> codec;
        const dstd::Vector<std::uint32_t> values = {1, 2};
        dstd::Vector<std::byte> bytes(9);
        auto written = codec.serialize(values, dstd::Span<std::byte>(bytes));
        REQUIRE_FALSE(written.has_value());
        CHECK(written.error().is(dstd::Condition::InvalidArgument));
    }
}
