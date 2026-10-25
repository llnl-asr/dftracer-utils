#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/key.h>
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <tuple>

namespace {

using dftracer::utils::ByteView;
using dftracer::utils::index::KeyBuilder;
using dftracer::utils::index::KeyReader;

constexpr std::uint64_t RANDOM_SEED = 0x5EED1234C0DEC0DEULL;
constexpr std::size_t PAIR_COUNT = 10000;

int compare_views(ByteView a, ByteView b) {
    const auto common = a.size() < b.size() ? a.size() : b.size();
    if (common != 0) {
        const int cmp = std::memcmp(a.data(), b.data(), common);
        if (cmp != 0) return cmp;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

template <typename T>
int compare_values(const T& a, const T& b) {
    if (a < b) return -1;
    if (b < a) return 1;
    return 0;
}

int compare_f64(double a, double b) {
    const bool a_nan = std::isnan(a);
    const bool b_nan = std::isnan(b);
    if (a_nan || b_nan) {
        if (a_nan && b_nan) return 0;
        return a_nan ? 1 : -1;
    }
    if (a < b) return -1;
    if (b < a) return 1;
    return 0;
}

int compare_string(const std::string& a, const std::string& b) {
    if (a < b) return -1;
    if (b < a) return 1;
    return 0;
}

template <typename T, typename Encode, typename Compare>
void check_pair(const T& a, const T& b, Encode encode, Compare compare) {
    KeyBuilder first;
    KeyBuilder second;
    encode(first, a);
    encode(second, b);
    const auto first_view = first.view();
    const auto second_view = second.view();
    const int encoded_order = compare_views(first_view, second_view);
    const int value_order = compare(a, b);
    CHECK((encoded_order < 0) == (value_order < 0));
    CHECK((encoded_order == 0) == (value_order == 0));
    CHECK((encoded_order > 0) == (value_order > 0));
}

template <typename T, typename Random, typename Encode, typename Compare>
void check_random_pairs(std::mt19937_64& rng, Random random, Encode encode,
                        Compare compare) {
    for (std::size_t i = 0; i < PAIR_COUNT; ++i) {
        check_pair(random(rng), random(rng), encode, compare);
    }
}

void encode_u8(KeyBuilder& builder, std::uint8_t value) { builder.u8(value); }

void encode_u16(KeyBuilder& builder, std::uint16_t value) {
    builder.u16(value);
}

void encode_u32(KeyBuilder& builder, std::uint32_t value) {
    builder.u32(value);
}

void encode_u64(KeyBuilder& builder, std::uint64_t value) {
    builder.u64(value);
}

void encode_i64(KeyBuilder& builder, std::int64_t value) { builder.i64(value); }

void encode_f64(KeyBuilder& builder, double value) { builder.f64(value); }

void encode_i8(KeyBuilder& builder, std::int8_t value) { builder.i8(value); }

void encode_i16(KeyBuilder& builder, std::int16_t value) { builder.i16(value); }

void encode_i32(KeyBuilder& builder, std::int32_t value) { builder.i32(value); }

void encode_f32(KeyBuilder& builder, float value) { builder.f32(value); }

int compare_f32(float a, float b) {
    return compare_f64(static_cast<double>(a), static_cast<double>(b));
}

void encode_string(KeyBuilder& builder, const std::string& value) {
    builder.str({value.data(), value.size()});
}

using Composite = std::tuple<std::uint32_t, std::string, std::int64_t>;

void encode_composite(KeyBuilder& builder, const Composite& value) {
    builder.u32(std::get<0>(value));
    builder.str({std::get<1>(value).data(), std::get<1>(value).size()});
    builder.i64(std::get<2>(value));
}

bool same_f64(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    if (a == 0.0 && b == 0.0) return true;
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

bool strings_equal(const dftracer::utils::dstd::String& a,
                   const std::string& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

std::string random_string(std::mt19937_64& rng) {
    std::uniform_int_distribution<unsigned> length_dist(0, 16);
    std::uniform_int_distribution<unsigned> byte_dist(0, 255);
    std::string out(static_cast<std::size_t>(length_dist(rng)), '\0');
    for (char& c : out) c = static_cast<char>(byte_dist(rng));
    return out;
}

Composite random_composite(std::mt19937_64& rng) {
    std::uniform_int_distribution<std::uint32_t> u32_dist;
    std::uniform_int_distribution<std::uint64_t> u64_dist;
    return {u32_dist(rng), random_string(rng),
            std::bit_cast<std::int64_t>(u64_dist(rng))};
}

}  // namespace

TEST_SUITE("index store key") {
    TEST_CASE("unsigned keys preserve bytewise order") {
        std::mt19937_64 rng(RANDOM_SEED);
        std::uniform_int_distribution<unsigned> u8_dist(0, 255);
        check_random_pairs<std::uint8_t>(
            rng,
            [&](std::mt19937_64& engine) {
                return static_cast<std::uint8_t>(u8_dist(engine));
            },
            encode_u8, compare_values<std::uint8_t>);

        std::uniform_int_distribution<unsigned> u16_dist(0, 0xFFFFU);
        check_random_pairs<std::uint16_t>(
            rng,
            [&](std::mt19937_64& engine) {
                return static_cast<std::uint16_t>(u16_dist(engine));
            },
            encode_u16, compare_values<std::uint16_t>);

        std::uniform_int_distribution<std::uint32_t> u32_dist;
        check_random_pairs<std::uint32_t>(
            rng, [&](std::mt19937_64& engine) { return u32_dist(engine); },
            encode_u32, compare_values<std::uint32_t>);

        std::uniform_int_distribution<std::uint64_t> u64_dist;
        check_random_pairs<std::uint64_t>(
            rng, [&](std::mt19937_64& engine) { return u64_dist(engine); },
            encode_u64, compare_values<std::uint64_t>);
    }

    TEST_CASE("signed keys preserve bytewise order") {
        std::mt19937_64 rng(RANDOM_SEED);
        std::uniform_int_distribution<std::uint64_t> bits_dist;
        check_random_pairs<std::int64_t>(
            rng,
            [&](std::mt19937_64& engine) {
                return std::bit_cast<std::int64_t>(bits_dist(engine));
            },
            encode_i64, compare_values<std::int64_t>);

        const std::array<std::int64_t, 5> edges = {
            std::numeric_limits<std::int64_t>::min(), -1, 0, 1,
            std::numeric_limits<std::int64_t>::max()};
        for (std::size_t i = 1; i < edges.size(); ++i) {
            check_pair(edges[i - 1], edges[i], encode_i64,
                       compare_values<std::int64_t>);
        }
    }

    TEST_CASE("double keys preserve order and canonicalize zeros and NaNs") {
        std::mt19937_64 rng(RANDOM_SEED);
        std::uniform_int_distribution<std::uint64_t> bits_dist;
        check_random_pairs<double>(
            rng,
            [&](std::mt19937_64& engine) {
                return std::bit_cast<double>(bits_dist(engine));
            },
            encode_f64, compare_f64);

        const double denormal =
            std::bit_cast<double>(std::uint64_t{0x0000000000000001ULL});
        const std::array<double, 9> edges = {
            -std::numeric_limits<double>::infinity(),
            -1.0,
            -0.0,
            0.0,
            denormal,
            1.0,
            std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::signaling_NaN()};
        for (std::size_t i = 1; i < edges.size(); ++i) {
            check_pair(edges[i - 1], edges[i], encode_f64, compare_f64);
        }
        check_pair(-0.0, 0.0, encode_f64, compare_f64);
        check_pair(std::numeric_limits<double>::quiet_NaN(),
                   std::numeric_limits<double>::signaling_NaN(), encode_f64,
                   compare_f64);
    }

    TEST_CASE("narrow signed keys preserve bytewise order") {
        std::mt19937_64 rng(RANDOM_SEED);
        std::uniform_int_distribution<std::uint64_t> bits_dist;
        check_random_pairs<std::int8_t>(
            rng,
            [&](std::mt19937_64& engine) {
                return static_cast<std::int8_t>(
                    static_cast<std::uint8_t>(bits_dist(engine)));
            },
            encode_i8, compare_values<std::int8_t>);
        check_random_pairs<std::int16_t>(
            rng,
            [&](std::mt19937_64& engine) {
                return static_cast<std::int16_t>(
                    static_cast<std::uint16_t>(bits_dist(engine)));
            },
            encode_i16, compare_values<std::int16_t>);
        check_random_pairs<std::int32_t>(
            rng,
            [&](std::mt19937_64& engine) {
                return static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(bits_dist(engine)));
            },
            encode_i32, compare_values<std::int32_t>);

        const std::array<std::int32_t, 5> edges = {
            std::numeric_limits<std::int32_t>::min(), -1, 0, 1,
            std::numeric_limits<std::int32_t>::max()};
        for (std::size_t i = 1; i < edges.size(); ++i) {
            check_pair(edges[i - 1], edges[i], encode_i32,
                       compare_values<std::int32_t>);
        }
        check_pair(std::numeric_limits<std::int8_t>::min(),
                   std::numeric_limits<std::int8_t>::max(), encode_i8,
                   compare_values<std::int8_t>);
        check_pair(std::numeric_limits<std::int16_t>::min(), std::int16_t{-1},
                   encode_i16, compare_values<std::int16_t>);
    }

    TEST_CASE("float keys preserve order and canonicalize zeros and NaNs") {
        std::mt19937_64 rng(RANDOM_SEED);
        std::uniform_int_distribution<std::uint32_t> bits_dist;
        check_random_pairs<float>(
            rng,
            [&](std::mt19937_64& engine) {
                return std::bit_cast<float>(bits_dist(engine));
            },
            encode_f32, compare_f32);

        const float denormal = std::bit_cast<float>(std::uint32_t{1});
        const std::array<float, 9> edges = {
            -std::numeric_limits<float>::infinity(),
            -1.0F,
            -0.0F,
            0.0F,
            denormal,
            1.0F,
            std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::signaling_NaN()};
        for (std::size_t i = 1; i < edges.size(); ++i) {
            check_pair(edges[i - 1], edges[i], encode_f32, compare_f32);
        }
    }

    TEST_CASE("narrow signed and float parts round-trip through KeyReader") {
        KeyBuilder builder;
        builder.i8(-128).i8(127).i16(-32768).i16(12345).i32(-7).i32(
            std::numeric_limits<std::int32_t>::max());
        builder.f32(-2.5F).f32(0.0F).f32(
            std::numeric_limits<float>::infinity());
        KeyReader reader(builder.view());
        CHECK(*reader.i8() == -128);
        CHECK(*reader.i8() == 127);
        CHECK(*reader.i16() == -32768);
        CHECK(*reader.i16() == 12345);
        CHECK(*reader.i32() == -7);
        CHECK(*reader.i32() == std::numeric_limits<std::int32_t>::max());
        CHECK(*reader.f32() == -2.5F);
        CHECK(*reader.f32() == 0.0F);
        CHECK(*reader.f32() == std::numeric_limits<float>::infinity());
        CHECK(reader.done());
        CHECK(!KeyReader(builder.view().subspan(0, 2)).i32());
        CHECK(!KeyReader(builder.view().subspan(0, 3)).f32());
    }

    TEST_CASE("string keys preserve order with embedded zeros") {
        std::mt19937_64 rng(RANDOM_SEED);
        check_random_pairs<std::string>(
            rng, [](std::mt19937_64& engine) { return random_string(engine); },
            encode_string, compare_string);

        const std::array<std::string, 6> edges = {
            std::string(""),        std::string("a"),
            std::string("ab"),      std::string("a\0", 2),
            std::string("a\0b", 3), std::string("a\xFF", 2)};
        for (std::size_t i = 1; i < edges.size(); ++i) {
            check_pair(edges[i - 1], edges[i], encode_string, compare_string);
        }
    }

    TEST_CASE("composite keys order lexicographically by part") {
        std::mt19937_64 rng(RANDOM_SEED);
        check_random_pairs<Composite>(
            rng,
            [](std::mt19937_64& engine) { return random_composite(engine); },
            encode_composite, compare_values<Composite>);
    }

    TEST_CASE("every part round-trips through KeyReader") {
        {
            KeyBuilder builder;
            builder.u8(0x12);
            KeyReader reader(builder.view());
            const auto value = reader.u8();
            REQUIRE(value.has_value());
            CHECK(*value == 0x12);
            CHECK(reader.done());
        }
        {
            KeyBuilder builder;
            builder.u16(0x1234);
            KeyReader reader(builder.view());
            const auto value = reader.u16();
            REQUIRE(value.has_value());
            CHECK(*value == 0x1234);
            CHECK(reader.done());
        }
        {
            KeyBuilder builder;
            builder.u32(0x12345678U);
            KeyReader reader(builder.view());
            const auto value = reader.u32();
            REQUIRE(value.has_value());
            CHECK(*value == 0x12345678U);
            CHECK(reader.done());
        }
        {
            KeyBuilder builder;
            builder.u64(0x123456789ABCDEF0ULL);
            KeyReader reader(builder.view());
            const auto value = reader.u64();
            REQUIRE(value.has_value());
            CHECK(*value == 0x123456789ABCDEF0ULL);
            CHECK(reader.done());
        }
        {
            KeyBuilder builder;
            builder.i64(std::numeric_limits<std::int64_t>::min());
            KeyReader reader(builder.view());
            const auto value = reader.i64();
            REQUIRE(value.has_value());
            CHECK(*value == std::numeric_limits<std::int64_t>::min());
            CHECK(reader.done());
        }
        {
            KeyBuilder builder;
            builder.f64(-12.5);
            KeyReader reader(builder.view());
            const auto value = reader.f64();
            REQUIRE(value.has_value());
            CHECK(same_f64(*value, -12.5));
            CHECK(reader.done());
        }
        {
            const std::string original("a\0b", 3);
            KeyBuilder builder;
            builder.str({original.data(), original.size()});
            KeyReader reader(builder.view());
            const auto value = reader.str();
            REQUIRE(value.has_value());
            CHECK(strings_equal(*value, original));
            CHECK(reader.done());
        }
    }

    TEST_CASE("random values round-trip through KeyReader") {
        std::mt19937_64 rng(RANDOM_SEED);
        std::uniform_int_distribution<std::uint64_t> bits_dist;
        for (std::size_t i = 0; i < PAIR_COUNT; ++i) {
            {
                const auto original = bits_dist(rng);
                KeyBuilder builder;
                builder.u64(original);
                KeyReader reader(builder.view());
                const auto value = reader.u64();
                REQUIRE(value.has_value());
                CHECK(*value == original);
            }
            {
                const auto original =
                    std::bit_cast<std::int64_t>(bits_dist(rng));
                KeyBuilder builder;
                builder.i64(original);
                KeyReader reader(builder.view());
                const auto value = reader.i64();
                REQUIRE(value.has_value());
                CHECK(*value == original);
            }
            {
                const auto original = std::bit_cast<double>(bits_dist(rng));
                KeyBuilder builder;
                builder.f64(original);
                KeyReader reader(builder.view());
                const auto value = reader.f64();
                REQUIRE(value.has_value());
                CHECK(same_f64(*value, original));
            }
            {
                const auto original = random_string(rng);
                KeyBuilder builder;
                builder.str({original.data(), original.size()});
                KeyReader reader(builder.view());
                const auto value = reader.str();
                REQUIRE(value.has_value());
                CHECK(strings_equal(*value, original));
            }
        }
    }

    TEST_CASE("raw is the unconsumed remainder") {
        const std::string tail = "tail";
        KeyBuilder builder;
        builder.u32(0x01020304U).raw({tail.data(), tail.size()});
        KeyReader reader(builder.view());
        const auto prefix = reader.u32();
        REQUIRE(prefix.has_value());
        CHECK(*prefix == 0x01020304U);
        const auto rest = reader.rest();
        REQUIRE(rest.size() == tail.size());
        CHECK(std::memcmp(rest.data(), tail.data(), tail.size()) == 0);
        CHECK(reader.done());
    }

    TEST_CASE("truncated and malformed keys return Parse") {
        using dftracer::utils::dstd::Condition;

        const auto check_parse = [](const auto& result) {
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().condition() == Condition::Parse);
            CHECK(result.error().is(Condition::Parse));
        };

        {
            KeyReader reader(ByteView{});
            check_parse(reader.u8());
        }
        {
            KeyBuilder builder;
            builder.u16(0x1234);
            KeyReader reader(builder.view().subspan(0, 1));
            check_parse(reader.u16());
        }
        {
            KeyBuilder builder;
            builder.u32(0x12345678U);
            KeyReader reader(builder.view().subspan(0, 3));
            check_parse(reader.u32());
        }
        {
            KeyBuilder builder;
            builder.u64(0x123456789ABCDEF0ULL);
            KeyReader reader(builder.view().subspan(0, 7));
            check_parse(reader.u64());
        }
        {
            KeyBuilder builder;
            builder.i64(-1);
            KeyReader reader(builder.view().subspan(0, 7));
            check_parse(reader.i64());
        }
        {
            KeyBuilder builder;
            builder.f64(1.5);
            KeyReader reader(builder.view().subspan(0, 7));
            check_parse(reader.f64());
        }
        {
            KeyReader reader(ByteView{});
            check_parse(reader.str());
        }
        {
            KeyBuilder builder;
            builder.str("abc");
            KeyReader reader(
                builder.view().subspan(0, builder.view().size() - 1));
            check_parse(reader.str());
        }
        {
            const std::array<std::uint8_t, 2> malformed = {0x00, 0x02};
            KeyReader reader(ByteView(malformed.data(), malformed.size()));
            check_parse(reader.str());
        }
        {
            const std::array<std::uint8_t, 1> truncated_escape = {0x00};
            KeyReader reader(
                ByteView(truncated_escape.data(), truncated_escape.size()));
            check_parse(reader.str());
        }
    }

    TEST_CASE("view size and clear track the owned bytes") {
        KeyBuilder builder;
        CHECK(builder.size() == 0);
        CHECK(builder.view().size() == 0);
        builder.u32(0x01020304U);
        CHECK(builder.size() == 4);
        CHECK(builder.view().size() == 4);
        const auto first = builder.view();
        CHECK(std::memcmp(first.data(), "\x01\x02\x03\x04", first.size()) == 0);
        builder.clear();
        CHECK(builder.size() == 0);
        CHECK(builder.view().size() == 0);
        builder.u8(0x7F);
        CHECK(builder.size() == 1);
        CHECK(builder.view().size() == 1);
    }
}
