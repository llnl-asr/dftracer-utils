#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/utilities/indexer/internal/column_type_codec.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>

using namespace dftracer::utils::dataframe;
using dftracer::utils::utilities::indexer::internal::decode_data_type;
using dftracer::utils::utilities::indexer::internal::encode_data_type;
using dftracer::utils::utilities::indexer::internal::merge_data_type;

namespace {

void check_round_trip(const DataType& type) {
    auto encoded = encode_data_type(type);
    auto decoded = decode_data_type(encoded);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == type);
}

}  // namespace

TEST_SUITE("column_type_codec") {
    TEST_CASE("round-trips a scalar") {
        check_round_trip(scalar(TypeId::Int64));
        check_round_trip(scalar(TypeId::String));
        check_round_trip(scalar(TypeId::Unknown));
    }

    TEST_CASE("round-trips List<Int64>") {
        check_round_trip(list_of(scalar(TypeId::Int64)));
    }

    TEST_CASE("round-trips Struct{a:Int64,b:String}") {
        check_round_trip(
            struct_of({Field{"a", scalar(TypeId::Int64), true},
                       Field{"b", scalar(TypeId::String), false}}));
    }

    TEST_CASE("round-trips Timestamp(Micro, UTC)") {
        check_round_trip(timestamp(TimeUnit::Micro, "UTC"));
    }

    TEST_CASE("round-trips a naive Timestamp (no timezone)") {
        check_round_trip(timestamp(TimeUnit::Nano));
    }

    TEST_CASE("round-trips Decimal128(38, 9)") {
        check_round_trip(decimal128(38, 9));
    }

    TEST_CASE("round-trips a Decimal with negative scale") {
        check_round_trip(decimal128(10, -2));
    }

    TEST_CASE("round-trips FixedSizeList(4, Float64)") {
        check_round_trip(fixed_size_list_of(scalar(TypeId::Float64), 4));
    }

    TEST_CASE("round-trips FixedSizeBinary(16)") {
        check_round_trip(fixed_size_binary(16));
    }

    TEST_CASE("round-trips a nested List<Struct{ts, amt}>") {
        DataType entry = struct_of(
            {Field{"ts", timestamp(TimeUnit::Milli, "America/Denver"), true},
             Field{"amt", decimal128(20, 4), true}});
        check_round_trip(list_of(entry));
    }

    TEST_CASE("round-trip fails if a parameter is dropped") {
        // Guard against the encoder silently forgetting the time unit: force
        // the encoded byte to a different unit and confirm the round-trip no
        // longer matches, so this test would have caught the bug.
        DataType type = timestamp(TimeUnit::Micro, "UTC");
        std::string encoded = encode_data_type(type);
        REQUIRE(encoded.size() >= 1);
        std::string tampered = encoded;
        // The time unit is the second byte for Timestamp (after the type id).
        REQUIRE(tampered.size() >= 2);
        tampered[1] =
            static_cast<char>(static_cast<std::uint8_t>(TimeUnit::Nano));
        auto decoded = decode_data_type(tampered);
        REQUIRE(decoded.has_value());
        CHECK(decoded->time_unit != type.time_unit);
        CHECK(*decoded != type);
    }

    TEST_CASE("decode rejects truncated input") {
        auto encoded = encode_data_type(
            struct_of({Field{"a", scalar(TypeId::Int64), true}}));
        for (std::size_t len = 0; len < encoded.size(); ++len) {
            auto decoded =
                decode_data_type(std::string_view(encoded).substr(0, len));
            CHECK_FALSE(decoded.has_value());
        }
    }

    TEST_CASE("decode rejects an unknown type id") {
        std::string bad;
        bad.push_back(static_cast<char>(200));  // far past the last TypeId
        CHECK_FALSE(decode_data_type(bad).has_value());
    }

    TEST_CASE("decode rejects an absurd field count") {
        std::string bad;
        bad.push_back(
            static_cast<char>(static_cast<std::uint8_t>(TypeId::Struct)));
        // A varint field count of a huge value (all continuation bits set).
        for (int i = 0; i < 9; ++i) bad.push_back(static_cast<char>(0xFF));
        bad.push_back(static_cast<char>(0x01));
        CHECK_FALSE(decode_data_type(bad).has_value());
    }

    TEST_CASE("decode rejects trailing garbage") {
        auto encoded = encode_data_type(scalar(TypeId::Int64));
        encoded.push_back('\xAB');
        CHECK_FALSE(decode_data_type(encoded).has_value());
    }

    TEST_CASE("merge widens Int64 and Float64 to Float64") {
        auto merged =
            merge_data_type(scalar(TypeId::Int64), scalar(TypeId::Float64));
        CHECK(merged == scalar(TypeId::Float64));
    }

    TEST_CASE("merge yields Unknown to the other observation") {
        auto merged =
            merge_data_type(scalar(TypeId::Unknown), scalar(TypeId::String));
        CHECK(merged == scalar(TypeId::String));
    }

    TEST_CASE("merge of two different nested shapes conflicts to String") {
        auto a = list_of(scalar(TypeId::Int64));
        auto b = struct_of({Field{"x", scalar(TypeId::Int64), true}});
        auto merged = merge_data_type(a, b);
        CHECK(merged == scalar(TypeId::String));
    }

    TEST_CASE("merge of identical nested shapes passes through unchanged") {
        auto a = list_of(scalar(TypeId::Int64));
        auto b = list_of(scalar(TypeId::Int64));
        CHECK(merge_data_type(a, b) == a);
    }
}
