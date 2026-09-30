#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

namespace {

bool bit(const Series& s, std::int64_t i) {
    return (s.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1;
}

}  // namespace

TEST_CASE("integers and floats cast to text") {
    std::vector<std::int64_t> ints{1, 2, -30};
    Series i = Series::flat_i64(ints.data(), 3);
    Series ti = i.cast(TypeId::String);
    REQUIRE(ti.valid());
    CHECK(ti.type() == TypeId::String);
    CHECK(ti.string_at(0) == "1");
    CHECK(ti.string_at(1) == "2");
    CHECK(ti.string_at(2) == "-30");

    std::vector<double> fl{1.5, 0.0, 2.0, 1e21, 0.1234567891};
    Series f = Series::flat_f64(fl.data(), 5);
    Series tf = f.cast(TypeId::String);
    REQUIRE(tf.valid());
    CHECK(tf.string_at(0) == "1.5");
    CHECK(tf.string_at(1) == "0.0");
    CHECK(tf.string_at(2) == "2.0");
    CHECK(tf.string_at(3) == "1e+21");
    CHECK(tf.string_at(4) == "0.1234567891");
}

TEST_CASE("a null stays null when cast to text") {
    std::vector<std::int64_t> ints{7, 0, 9};
    std::vector<std::uint8_t> valid{0x05};  // row 1 is null
    Series i = Series::flat_i64(ints.data(), 3, valid.data());
    Series t = i.cast(TypeId::String);
    REQUIRE(t.valid());
    CHECK_FALSE(t.is_null(0));
    CHECK(t.is_null(1));
    CHECK(t.string_at(2) == "9");
}

TEST_CASE("bool casts to true and false") {
    std::vector<std::uint8_t> bits{0x05};  // true, false, true
    Series b = Series::flat(TypeId::Bool, bits.data(), 3);
    Series t = b.cast(TypeId::String);
    REQUIRE(t.valid());
    CHECK(t.string_at(0) == "true");
    CHECK(t.string_at(1) == "false");
    CHECK(t.string_at(2) == "true");
}

TEST_CASE("numbers cast to bool: nonzero true, NaN and null null") {
    std::vector<std::int64_t> ints{1, 0, -3};
    Series bi = Series::flat_i64(ints.data(), 3).cast(TypeId::Bool);
    REQUIRE(bi.valid());
    CHECK(bi.type() == TypeId::Bool);
    CHECK(bit(bi, 0));
    CHECK_FALSE(bit(bi, 1));
    CHECK(bit(bi, 2));
    CHECK(bi.null_count() == 0);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> fl{1.5, 0.0, nan, 4.0};
    std::vector<std::uint8_t> valid{0x07};  // row 3 is null
    Series bf = Series::flat_f64(fl.data(), 4, valid.data()).cast(TypeId::Bool);
    REQUIRE(bf.valid());
    CHECK(bit(bf, 0));
    CHECK_FALSE(bit(bf, 1));
    CHECK(bf.is_null(2));
    CHECK(bf.is_null(3));
    CHECK(bf.null_count() == 2);
}

TEST_CASE("an unsupported pair is refused; a same-type cast is the column") {
    std::vector<std::string_view> words{"a", "b"};
    Series s = Series::strings(std::span<const std::string_view>(words));
    CHECK_FALSE(s.cast(TypeId::Bool).valid());
    Series same = s.cast(TypeId::String);
    REQUIRE(same.valid());
    CHECK(same.string_at(1) == "b");
}

TEST_CASE("numeric to numeric casts still match static_cast") {
    std::mt19937_64 rng(7);
    std::vector<double> fl(1000);
    for (auto& v : fl)
        v = std::uniform_real_distribution<double>(-1e6, 1e6)(rng);
    Series f = Series::flat_f64(fl.data(), 1000);
    Series i = f.cast(TypeId::Int64);
    Series back = i.cast(TypeId::Float64);
    REQUIRE(i.valid());
    REQUIRE(back.valid());
    for (std::int64_t k = 0; k < 1000; ++k) {
        CHECK(i.data<std::int64_t>()[k] == static_cast<std::int64_t>(fl[k]));
        CHECK(back.data<double>()[k] ==
              static_cast<double>(static_cast<std::int64_t>(fl[k])));
    }
}
