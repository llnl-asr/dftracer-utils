#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <doctest/doctest.h>

#include <cstdint>

using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

TEST_CASE("a column of nulls has the frame's length and type") {
    Series n = Series::nulls(TypeId::Int64, 4);
    REQUIRE(n.valid());
    CHECK(n.type() == TypeId::Int64);
    CHECK(n.length() == 4);
    CHECK(n.null_count() == 4);
}

TEST_CASE("a column of nulls can be empty and can be a string column") {
    Series e = Series::nulls(TypeId::Int64, 0);
    REQUIRE(e.valid());
    CHECK(e.length() == 0);
    Series s = Series::nulls(TypeId::String, 3);
    REQUIRE(s.valid());
    CHECK(s.type() == TypeId::String);
    CHECK(s.is_null(2));
}
