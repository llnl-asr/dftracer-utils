#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace dftracer::utils;

TEST_SUITE("little_endian") {
    TEST_CASE("little_endian - exact byte layout") {
        std::uint8_t b[8] = {};
        write_u16_le(b, 0x0102);
        CHECK(b[0] == 0x02);
        CHECK(b[1] == 0x01);
        write_u32_le(b, 0x01020304U);
        CHECK(b[0] == 0x04);
        CHECK(b[3] == 0x01);
        write_u64_le(b, 0x0102030405060708ULL);
        for (int i = 0; i < 8; ++i) CHECK(b[i] == 8 - i);
        CHECK(read_u64_le(b) == 0x0102030405060708ULL);
        CHECK(read_u32_le(b) == 0x05060708U);
        CHECK(read_u16_le(b) == 0x0708);
    }

    TEST_CASE("little_endian - integer round trips") {
        std::uint8_t b[8];
        for (std::uint16_t v :
             {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0xFFFF}}) {
            write_u16_le(b, v);
            CHECK(read_u16_le(b) == v);
        }
        for (std::uint32_t v : {0U, 1U, 0x80000000U, 0xFFFFFFFFU}) {
            write_u32_le(b, v);
            CHECK(read_u32_le(b) == v);
        }
        for (std::uint64_t v : {std::uint64_t{0}, std::uint64_t{1},
                                std::uint64_t{1} << 63, ~std::uint64_t{0}}) {
            write_u64_le(b, v);
            CHECK(read_u64_le(b) == v);
        }
    }

    TEST_CASE("little_endian - double keeps the bit pattern") {
        std::uint8_t b[8];
        const double values[] = {0.0,
                                 -0.0,
                                 1.5,
                                 -1.5,
                                 std::numeric_limits<double>::max(),
                                 std::numeric_limits<double>::lowest(),
                                 std::numeric_limits<double>::denorm_min(),
                                 std::numeric_limits<double>::infinity()};
        for (double v : values) {
            write_f64_le(b, v);
            const double back = read_f64_le(b);
            CHECK(double_bits(back) == double_bits(v));
        }
        write_f64_le(b, -0.0);
        CHECK(std::signbit(read_f64_le(b)));

        const std::uint64_t nan_bits = 0x7FF8000000000123ULL;
        write_u64_le(b, nan_bits);
        const double nan = read_f64_le(b);
        CHECK(std::isnan(nan));
        write_f64_le(b, nan);
        CHECK(read_u64_le(b) == nan_bits);
    }

    TEST_CASE("little_endian - append grows and keeps order") {
        dstd::Vector<std::uint8_t> v;
        append_u16_le(v, 0x0201);
        CHECK(v.size() == 2);
        append_u32_le(v, 0x06050403U);
        CHECK(v.size() == 6);
        append_u64_le(v, 0x0E0D0C0B0A090807ULL);
        CHECK(v.size() == 14);
        append_f64_le(v, 1.0);
        CHECK(v.size() == 22);
        for (std::size_t i = 0; i < 14; ++i) CHECK(v[i] == i + 1);
        CHECK(read_f64_le(v.data() + 14) == 1.0);

        std::vector<std::uint8_t> plain;
        append_u32_le(plain, 7U);
        CHECK(plain.size() == 4);
        CHECK(read_u32_le(plain.data()) == 7U);
    }

    TEST_CASE("little_endian - reader walks the buffer") {
        dstd::Vector<std::uint8_t> v;
        v.push_back(0xAB);
        append_u16_le(v, 0xBEEF);
        append_u32_le(v, 0xDEADBEEFU);
        append_u64_le(v, 0x0123456789ABCDEFULL);
        append_f64_le(v, -2.25);
        append_u32_le(v, 0xFFFFFFFEU);

        LeReader r(v.data(), v.size());
        std::uint8_t a = 0;
        std::uint16_t b = 0;
        std::uint32_t c = 0;
        std::uint64_t d = 0;
        double e = 0;
        std::int32_t f = 0;
        CHECK(r.u8(a));
        CHECK(r.u16(b));
        CHECK(r.u32(c));
        CHECK(r.u64(d));
        CHECK(r.f64(e));
        CHECK(r.i32(f));
        CHECK(a == 0xAB);
        CHECK(b == 0xBEEF);
        CHECK(c == 0xDEADBEEFU);
        CHECK(d == 0x0123456789ABCDEFULL);
        CHECK(e == -2.25);
        CHECK(f == -2);
        CHECK(r.remaining() == 0);
    }

    TEST_CASE("little_endian - reader refuses a short buffer") {
        const std::uint8_t data[6] = {1, 2, 3, 4, 5, 6};
        LeReader r(data, sizeof data);
        std::uint64_t d = 99;
        CHECK_FALSE(r.u64(d));
        CHECK(d == 99);
        CHECK(r.remaining() == 6);

        std::uint32_t c = 0;
        CHECK(r.u32(c));
        CHECK(c == 0x04030201U);
        CHECK(r.remaining() == 2);
        CHECK_FALSE(r.u32(c));
        CHECK(r.remaining() == 2);
        CHECK(r.take(3) == nullptr);
        CHECK(r.take(2) == data + 4);
        std::uint8_t a = 0;
        CHECK_FALSE(r.u8(a));
    }
}
