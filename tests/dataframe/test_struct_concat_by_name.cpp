// Struct columns concatenate by field name, not by position.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/rest_column.h>
#include <dftracer/utils/dataframe/series.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::dataframe::concat;
using dftracer::utils::dataframe::concat_columns;
using dftracer::utils::dataframe::ConcatHow;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

namespace {

Series i64s(const std::vector<std::int64_t>& v) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()),
                            nullptr);
}

Series f64s(const std::vector<double>& v) {
    return Series::flat_f64(v.data(), static_cast<std::int64_t>(v.size()),
                            nullptr);
}

Series st(std::vector<std::string> names, std::vector<Series> f) {
    return Series::structs(std::move(names), std::move(f));
}

std::vector<Series> one(Series s) {
    std::vector<Series> v;
    v.push_back(std::move(s));
    return v;
}

std::vector<Series> two(Series a, Series b) {
    std::vector<Series> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    return v;
}

std::int64_t field_index(const Series& s, const std::string& name) {
    for (std::int64_t f = 0; f < s.num_children(); ++f)
        if (s.field_name(f) == name) return f;
    return -1;
}

}  // namespace

TEST_SUITE("struct concat by name") {
    TEST_CASE("disjoint fields fill the missing side with nulls") {
        Series a = st({"a", "b"}, two(i64s({1, 2}), i64s({10, 20})));
        Series b = st({"c"}, one(i64s({7})));
        Series out = concat_columns({&a, &b});
        REQUIRE(out.length() == 3);
        REQUIRE(out.num_children() == 3);
        CHECK(out.field_name(0) == "a");
        CHECK(out.field_name(1) == "b");
        CHECK(out.field_name(2) == "c");
        const Series ca = out.child(0);
        const Series cc = out.child(2);
        CHECK_FALSE(ca.is_null(0));
        CHECK_FALSE(ca.is_null(1));
        CHECK(ca.is_null(2));
        CHECK(cc.is_null(0));
        CHECK(cc.is_null(1));
        CHECK_FALSE(cc.is_null(2));
        CHECK(cc.data<std::int64_t>()[2] == 7);
    }

    TEST_CASE("reordered fields match by name") {
        Series a = st({"a", "b"}, two(i64s({1, 2}), i64s({10, 20})));
        Series b = st({"b", "a"}, two(i64s({30}), i64s({3})));
        Series out = concat_columns({&a, &b});
        REQUIRE(out.length() == 3);
        REQUIRE(out.num_children() == 2);
        const Series ca = out.child(field_index(out, "a"));
        const Series cb = out.child(field_index(out, "b"));
        CHECK(ca.data<std::int64_t>()[2] == 3);
        CHECK(cb.data<std::int64_t>()[2] == 30);
        CHECK(cb.data<std::int64_t>()[0] == 10);
    }

    TEST_CASE("a field of two numeric types widens to Float64") {
        Series a = st({"a"}, one(i64s({1})));
        Series b = st({"a"}, one(f64s({2.5})));
        Series out = concat_columns({&a, &b});
        const Series ca = out.child(0);
        REQUIRE(ca.type() == TypeId::Float64);
        CHECK(ca.data<double>()[0] == 1.0);
        CHECK(ca.data<double>()[1] == 2.5);
    }

    TEST_CASE("int64 against string follows the diagonal concat") {
        Series a = st({"a"}, one(i64s({1})));
        Series b = st({"a"}, one(Series::strings({"x"})));
        Series out = concat_columns({&a, &b});
        DataFrame fa, fb;
        fa.names = {"a"};
        fa.columns.push_back(i64s({1}));
        fb.names = {"a"};
        fb.columns.push_back(Series::strings({"x"}));
        DataFrame diag = concat({&fa, &fb}, ConcatHow::Diagonal);
        const Series ca = out.child(0);
        CHECK(ca.type() == diag.columns.front().type());
        CHECK(ca.is_json() == diag.columns.front().is_json());
        CHECK(ca.length() == 2);
    }

    TEST_CASE("struct-level nulls survive") {
        Series a = st({"a"}, one(i64s({1, 2}))).take({0, -1});
        Series b = st({"a"}, one(i64s({3})));
        Series out = concat_columns({&a, &b});
        REQUIRE(out.length() == 3);
        CHECK_FALSE(out.is_null(0));
        CHECK(out.is_null(1));
        CHECK_FALSE(out.is_null(2));
    }

    TEST_CASE("a zero-field part keeps its rows as nulls") {
        Series a = dftracer::utils::dataframe::struct_of_length({}, {}, 3);
        Series b = st({"a"}, one(i64s({5, 6})));
        Series out = concat_columns({&a, &b});
        REQUIRE(out.num_children() == 1);
        REQUIRE(out.length() == 5);
        for (std::int64_t i = 0; i < 3; ++i) CHECK(out.child(0).is_null(i));
        CHECK(out.child(0).data<std::int64_t>()[4] == 6);
        Series empty = concat_columns({&a, &a});
        CHECK(empty.num_children() == 0);
        CHECK(empty.length() == 6);
    }

    TEST_CASE("equal fields concatenate as before") {
        Series a = st({"a", "b"}, two(i64s({1}), i64s({2})));
        Series b = st({"a", "b"}, two(i64s({3}), i64s({4})));
        Series out = concat_columns({&a, &b});
        REQUIRE(out.length() == 2);
        CHECK(out.child(0).data<std::int64_t>()[1] == 3);
        CHECK(out.child(1).data<std::int64_t>()[1] == 4);
    }
}
