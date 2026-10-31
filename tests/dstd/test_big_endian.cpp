#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace dftracer::utils;

TEST_SUITE("big_endian") {
    TEST_CASE("big_endian - exact byte layout") {
        std::uint8_t b[8] = {};
        dstd::write_u16_be(b, 0x0102);
        CHECK(b[0] == 0x01);
        CHECK(b[1] == 0x02);
        dstd::write_u32_be(b, 0x01020304U);
        CHECK(b[0] == 0x01);
        CHECK(b[3] == 0x04);
        dstd::write_u64_be(b, 0x0102030405060708ULL);
        for (int i = 0; i < 8; ++i) CHECK(b[i] == i + 1);
        CHECK(dstd::read_u64_be(b) == 0x0102030405060708ULL);
        CHECK(dstd::read_u32_be(b) == 0x01020304U);
        CHECK(dstd::read_u16_be(b) == 0x0102);
    }

    TEST_CASE("big_endian - integer and double round trips") {
        std::uint8_t b[8];
        for (std::uint64_t v : {std::uint64_t{0}, std::uint64_t{1},
                                std::uint64_t{0x8000000000000000ULL},
                                std::numeric_limits<std::uint64_t>::max()}) {
            dstd::write_u64_be(b, v);
            CHECK(dstd::read_u64_be(b) == v);
        }
        for (double d : {0.0, -0.0, 1.5, -1e300, 1e-300}) {
            dstd::write_f64_be(b, d);
            CHECK(dstd::double_bits(dstd::read_f64_be(b)) ==
                  dstd::double_bits(d));
        }
        dstd::write_f64_be(b, 1.0);
        CHECK(b[0] == 0x3F);
        CHECK(b[1] == 0xF0);
    }

    TEST_CASE("big_endian - append to std::string and vectors") {
        std::string s;
        dstd::append_u16_be(s, 0xBEEF);
        dstd::append_u32_be(s, 0x01020304U);
        dstd::append_u64_be(s, 0x0102030405060708ULL);
        REQUIRE(s.size() == 14);
        CHECK(static_cast<unsigned char>(s[0]) == 0xBE);
        CHECK(static_cast<unsigned char>(s[1]) == 0xEF);
        CHECK(static_cast<unsigned char>(s[2]) == 0x01);
        CHECK(static_cast<unsigned char>(s[13]) == 0x08);

        std::vector<std::uint8_t> v;
        dstd::append_f64_be(v, 1.0);
        REQUIRE(v.size() == 8);
        CHECK(v[0] == 0x3F);
        CHECK(dstd::read_f64_be(v.data()) == 1.0);

        dstd::Vector<std::uint8_t> d;
        dstd::append_u32_be(d, 7);
        REQUIRE(d.size() == 4);
        CHECK(d[3] == 7);
    }

    TEST_CASE("big_endian - BeReader refuses short input") {
        const std::uint8_t b[] = {0, 0, 0, 1, 0xFF};
        dstd::BeReader r(b, sizeof b);
        std::uint32_t u32 = 0;
        CHECK(r.u32(u32));
        CHECK(u32 == 1);
        std::uint16_t u16 = 99;
        CHECK_FALSE(r.u16(u16));
        CHECK(u16 == 99);
        CHECK(r.remaining() == 1);
        std::uint8_t u8 = 0;
        CHECK(r.u8(u8));
        CHECK(u8 == 0xFF);
        CHECK(r.take(1) == nullptr);
        std::uint64_t u64 = 0;
        CHECK_FALSE(r.u64(u64));
    }

    TEST_CASE("varint - byte layout") {
        struct Case {
            std::uint64_t value;
            std::vector<std::uint8_t> bytes;
        };
        const Case cases[] = {
            {0, {0x00}},
            {127, {0x7F}},
            {128, {0x80, 0x01}},
            {16383, {0xFF, 0x7F}},
            {16384, {0x80, 0x80, 0x01}},
            {std::numeric_limits<std::uint64_t>::max(),
             {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}},
        };
        for (const auto& c : cases) {
            std::string s;
            dstd::append_varint(s, c.value);
            REQUIRE(s.size() == c.bytes.size());
            for (std::size_t i = 0; i < s.size(); ++i)
                CHECK(static_cast<std::uint8_t>(s[i]) == c.bytes[i]);

            std::uint8_t buf[dstd::VARINT_MAX_BYTES];
            CHECK(dstd::write_varint(buf, c.value) - buf ==
                  static_cast<std::ptrdiff_t>(c.bytes.size()));

            const std::uint8_t* p = c.bytes.data();
            std::uint64_t out = 1;
            REQUIRE(dstd::read_varint(p, c.bytes.data() + c.bytes.size(), out));
            CHECK(out == c.value);
            CHECK(p == c.bytes.data() + c.bytes.size());
        }
    }

    TEST_CASE("varint - truncated and over-long input is refused") {
        const std::uint8_t cut[] = {0x80, 0x80};
        const std::uint8_t* p = cut;
        std::uint64_t out = 5;
        CHECK_FALSE(dstd::read_varint(p, cut + sizeof cut, out));
        CHECK(p == cut);
        CHECK(out == 5);
        CHECK_FALSE(dstd::read_varint(p, cut, out));

        const std::uint8_t over[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                     0xFF, 0xFF, 0xFF, 0xFF, 0x02};
        p = over;
        CHECK_FALSE(dstd::read_varint(p, over + sizeof over, out));

        const std::uint8_t two[] = {0x80, 0x01, 0x05};
        dstd::BeReader r(two, sizeof two);
        std::uint64_t a = 0;
        CHECK(r.varint(a));
        CHECK(a == 128);
        CHECK(r.varint(a));
        CHECK(a == 5);
        CHECK_FALSE(r.varint(a));
    }

    TEST_CASE("varint - size at the group boundaries") {
        struct Case {
            std::uint64_t value;
            std::size_t bytes;
        };
        const Case cases[] = {{0, 1},
                              {127, 1},
                              {128, 2},
                              {std::uint64_t{1} << 63, 10},
                              {std::numeric_limits<std::uint64_t>::max(), 10}};
        for (const auto& c : cases) {
            CHECK(dstd::varint_size(c.value) == c.bytes);
            std::string s;
            dstd::append_varint(s, c.value);
            CHECK(s.size() == c.bytes);
        }
    }

    TEST_CASE("varint - zigzag round trip") {
        const std::int64_t values[] = {
            0, -1, 1, std::numeric_limits<std::int64_t>::min(),
            std::numeric_limits<std::int64_t>::max()};
        for (const std::int64_t v : values) {
            CHECK(dstd::zigzag_decode(dstd::zigzag_encode(v)) == v);
        }
        CHECK(dstd::zigzag_encode(0) == 0);
        CHECK(dstd::zigzag_encode(-1) == 1);
        CHECK(dstd::zigzag_encode(1) == 2);
        CHECK(dstd::zigzag_encode(std::numeric_limits<std::int64_t>::min()) ==
              std::numeric_limits<std::uint64_t>::max());
        CHECK(dstd::zigzag_encode(std::numeric_limits<std::int64_t>::max()) ==
              std::numeric_limits<std::uint64_t>::max() - 1);
    }

    TEST_CASE("varint - LeReader overload refuses a truncated buffer") {
        const std::uint8_t cut[] = {0x80, 0x80};
        dstd::LeReader bad(cut, sizeof cut);
        std::uint64_t value = 5;
        CHECK_FALSE(dstd::read_varint(bad, value));
        CHECK(value == 5);
        CHECK(bad.remaining() == sizeof cut);
        CHECK(bad.position() == cut);

        const std::uint8_t two[] = {0x80, 0x01, 0x05};
        dstd::LeReader r(two, sizeof two);
        REQUIRE(dstd::read_varint(r, value));
        CHECK(value == 128);
        REQUIRE(dstd::read_varint(r, value));
        CHECK(value == 5);
        CHECK(r.remaining() == 0);
        CHECK_FALSE(dstd::read_varint(r, value));
    }
}
