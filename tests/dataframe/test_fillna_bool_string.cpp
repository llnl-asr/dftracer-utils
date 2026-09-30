#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using dftracer::utils::dataframe::Series;

namespace {

bool bit(const Series& s, std::int64_t i) {
    return (static_cast<const std::uint8_t*>(s.data<void>())[i >> 3] >>
            (i & 7)) &
           1;
}

dftu_scalar str_scalar(const char* t) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_STR;
    s.len = static_cast<std::uint32_t>(std::string_view(t).size());
    s.value.s = t;
    return s;
}

dftu_scalar i64_scalar(std::int64_t v) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = v;
    return s;
}

}  // namespace

TEST_CASE("fillna fills a bool column") {
    // False, null, True
    std::uint8_t bits = 0b100;
    std::uint8_t valid = 0b101;
    Series b = Series::flat(dftracer::utils::dataframe::TypeId::Bool, &bits, 3,
                            &valid);
    Series t = b.fillna(i64_scalar(1));
    REQUIRE(t.valid());
    CHECK(t.null_count() == 0);
    CHECK_FALSE(bit(t, 0));
    CHECK(bit(t, 1));
    CHECK(bit(t, 2));
    Series f = b.fillna(i64_scalar(0));
    REQUIRE(f.valid());
    CHECK_FALSE(bit(f, 1));
    CHECK(bit(f, 2));
}

TEST_CASE("fillna fills a string column") {
    const std::vector<std::string_view> rows = {"a", "", "c"};
    const std::uint8_t valid = 0b101;
    Series s = Series::strings(std::span<const std::string_view>(rows), &valid);
    Series r = s.fillna(str_scalar("x"));
    REQUIRE(r.valid());
    CHECK(r.null_count() == 0);
    CHECK(r.string_at(0) == "a");
    CHECK(r.string_at(1) == "x");
    CHECK(r.string_at(2) == "c");
}

TEST_CASE("a fill of the wrong kind is refused") {
    const std::vector<std::string_view> rows = {"a"};
    Series s = Series::strings(std::span<const std::string_view>(rows));
    CHECK_FALSE(s.fillna(i64_scalar(1)).valid());
    std::uint8_t bits = 0;
    Series b = Series::flat(dftracer::utils::dataframe::TypeId::Bool, &bits, 1);
    CHECK_FALSE(b.fillna(str_scalar("x")).valid());
}
