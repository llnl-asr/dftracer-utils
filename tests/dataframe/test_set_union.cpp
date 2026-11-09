// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

const dstd::String SEP(1, '\x1e');

// Group a (rows 0..1) and b (row 2) over the key column `keys`.
DataFrame set_union_of(const Series& values,
                       const dstd::Vector<dstd::StringView>& keys) {
    Series g = Series::strings(dstd::Span<const dstd::StringView>(keys));
    dstd::Vector<const Series*> inputs = {&g, &values};
    return dataframe::group_agg_expr(
        dataframe::expr_col(0),
        {dataframe::agg_set_union(dataframe::expr_col(1), "u")}, inputs, "g");
}

// The typed form over `values`: groups a (rows 0..1) and b (row 2).
DataFrame typed_of(const Series& values,
                   const dstd::Vector<dstd::StringView>& keys) {
    Series g = Series::strings(dstd::Span<const dstd::StringView>(keys));
    dstd::Vector<const Series*> inputs = {&g, &values};
    return dataframe::group_agg_expr(
        dataframe::expr_col(0),
        {dataframe::agg_set_union(dataframe::expr_col(1), "u", true)}, inputs,
        "g");
}

// The elements of row `row` of a list column, via its offsets.
template <class T>
dstd::Vector<T> list_cell(const DataFrame& r, std::int64_t row) {
    const Series& c = r.column("u");
    const Series child = c.child(0);
    dstd::Vector<T> out;
    for (std::int32_t k = c.offsets()[row]; k < c.offsets()[row + 1]; ++k)
        out.push_back(child.data<T>()[k]);
    return out;
}

dstd::String cell(const DataFrame& r, std::int64_t row) {
    return dstd::String(r.column("u").string_at(row));
}

}  // namespace

TEST_CASE("set_union of a list of integers is the union of the elements") {
    dstd::Vector<std::int64_t> elems{1, 2, 2, 3, 9};
    Series v = Series::list({0, 2, 4, 5}, Series::flat_i64(elems.data(), 5));
    DataFrame r = set_union_of(v, {"a", "a", "b"});
    REQUIRE(r.num_rows() == 2);
    CHECK(cell(r, 0) == "1" + SEP + "2" + SEP + "3");
    CHECK(cell(r, 1) == "9");
}

TEST_CASE("set_union of a list of strings is the union of the elements") {
    Series v = Series::list(
        {0, 2, 3, 4},
        Series::strings(dstd::Vector<dstd::String>{"p", "q", "q", "r"}));
    DataFrame r = set_union_of(v, {"a", "a", "b"});
    REQUIRE(r.num_rows() == 2);
    CHECK(cell(r, 0) == "p" + SEP + "q");
    CHECK(cell(r, 1) == "r");
}

TEST_CASE("null elements and empty lists contribute nothing") {
    // rows: [1, null], []; one group. A null list is covered in Python.
    dstd::Vector<std::int64_t> elems{1, 0};
    std::uint8_t elem_valid[1] = {0x01};
    Series v =
        Series::list({0, 2, 2}, Series::flat_i64(elems.data(), 2, elem_valid));
    DataFrame r = set_union_of(v, {"a", "a"});
    REQUIRE(r.num_rows() == 1);
    CHECK(cell(r, 0) == "1");
}

TEST_CASE("a column of an unsupported type is refused with its type named") {
    dstd::Vector<std::int64_t> field{1, 2, 3};
    dstd::Vector<Series> cols;
    cols.push_back(Series::flat_i64(field.data(), 3));
    Series st = Series::structs({"f"}, std::move(cols));
    CHECK_THROWS_WITH_AS(set_union_of(st, {"a", "a", "b"}),
                         doctest::Contains("struct"),
                         common::DFTUtilsException);
    dstd::Vector<std::int64_t> ts{1, 2, 3};
    Series t = Series::flat(dataframe::TypeId::Timestamp, ts.data(), 3);
    CHECK_THROWS_WITH_AS(set_union_of(t, {"a", "a", "b"}),
                         doctest::Contains("timestamp"),
                         common::DFTUtilsException);
}

TEST_CASE("a list of lists is refused with the element type named") {
    dstd::Vector<std::int64_t> leaf{1, 2, 3};
    Series inner = Series::list({0, 1, 3}, Series::flat_i64(leaf.data(), 3));
    Series outer = Series::list({0, 1, 2}, std::move(inner));
    CHECK_THROWS_WITH_AS(set_union_of(outer, {"a", "b"}),
                         doctest::Contains("list"), common::DFTUtilsException);
}

TEST_CASE("a float keeps every digit") {
    dstd::Vector<double> xs{0.1234567891, 0.5, 0.5};
    Series v = Series::flat_f64(xs.data(), 3);
    DataFrame r = set_union_of(v, {"a", "a", "b"});
    REQUIRE(r.num_rows() == 2);
    CHECK(cell(r, 0) == "0.1234567891" + SEP + "0.5");
    CHECK(cell(r, 1) == "0.5");
}

TEST_CASE("an empty string is a value") {
    Series v = Series::strings(dstd::Vector<dstd::String>{"x", "", ""});
    DataFrame r = set_union_of(v, {"a", "a", "b"});
    REQUIRE(r.num_rows() == 2);
    CHECK(cell(r, 0) == SEP + "x");  // "" sorts before "x"
    CHECK(cell(r, 1) == "");
}

TEST_CASE("a string holding the separator is refused") {
    Series v =
        Series::strings(dstd::Vector<dstd::String>{"x" + SEP + "y", "z"});
    CHECK_THROWS_WITH_AS(set_union_of(v, {"a", "a"}),
                         doctest::Contains("separator"),
                         common::DFTUtilsException);
}

TEST_CASE(
    "the other text aggregates refuse a list column instead of reading 0") {
    dstd::Vector<std::int64_t> elems{1, 2, 3};
    Series v = Series::list({0, 1, 3}, Series::flat_i64(elems.data(), 3));
    Series g = Series::strings(dstd::Vector<dstd::String>{"a", "b"});
    dstd::Vector<std::int64_t> by{5, 6};
    Series b = Series::flat_i64(by.data(), 2);
    dstd::Vector<const Series*> inputs = {&g, &v, &b};
    CHECK_THROWS_WITH_AS(
        dataframe::group_agg_expr(
            dataframe::expr_col(0),
            {dataframe::agg_argmax(dataframe::expr_col(1),
                                   dataframe::expr_col(2), "m")},
            inputs, "g"),
        doctest::Contains("list"), common::DFTUtilsException);
}

TEST_CASE("typed set_union orders integers by value, not by text") {
    dstd::Vector<std::int64_t> v{10, 9, 10};
    Series c = Series::flat_i64(v.data(), 3);
    DataFrame r = typed_of(c, {"a", "a", "b"});
    REQUIRE(r.num_rows() == 2);
    CHECK(r.column("u").type() == dataframe::TypeId::List);
    CHECK(r.column("u").child(0).type() == dataframe::TypeId::Int64);
    CHECK(list_cell<std::int64_t>(r, 0) == dstd::Vector<std::int64_t>{9, 10});
    CHECK(list_cell<std::int64_t>(r, 1) == dstd::Vector<std::int64_t>{10});
}

TEST_CASE("typed set_union keeps a string holding the separator") {
    Series c =
        Series::strings(dstd::Vector<dstd::String>{"b" + SEP + "c", "a", "a"});
    DataFrame r = typed_of(c, {"a", "a", "a"});
    REQUIRE(r.num_rows() == 1);
    const Series& u = r.column("u");
    CHECK(u.child(0).type() == dataframe::TypeId::String);
    CHECK(u.offsets()[1] == 2);
    CHECK(u.child(0).string_at(0) == "a");
    CHECK(u.child(0).string_at(1) == "b" + SEP + "c");
}

TEST_CASE("typed set_union of floats, unsigned integers and bools") {
    dstd::Vector<double> f{0.1, 1e300, 0.1, -0.0, 0.0, std::nan("")};
    Series fc = Series::flat_f64(f.data(), 6);
    DataFrame rf = typed_of(fc, {"a", "a", "a", "a", "a", "a"});
    CHECK(rf.column("u").child(0).type() == dataframe::TypeId::Float64);
    const dstd::Vector<double> got = list_cell<double>(rf, 0);
    REQUIRE(got.size() == 5);
    CHECK(std::signbit(got[0]));  // negative zero before zero
    CHECK(got[0] == 0.0);
    CHECK(got[1] == 0.0);
    CHECK(got[2] == 0.1);
    CHECK(got[3] == 1e300);
    CHECK(std::isnan(got[4]));  // NaN last

    dstd::Vector<std::uint64_t> u{18446744073709551615ull, 3, 3};
    Series uc = Series::flat(dataframe::TypeId::Uint64, u.data(), 3);
    DataFrame ru = typed_of(uc, {"a", "a", "a"});
    CHECK(list_cell<std::uint64_t>(ru, 0) ==
          dstd::Vector<std::uint64_t>{3, 18446744073709551615ull});

    const std::uint8_t bits[1] = {0x05};  // true, false, true
    Series bc = Series::flat(dataframe::TypeId::Bool, bits, 3);
    DataFrame rb = typed_of(bc, {"a", "a", "a"});
    CHECK(rb.column("u").child(0).type() == dataframe::TypeId::Bool);
    CHECK(rb.column("u").offsets()[1] == 2);
    const std::uint8_t packed = rb.column("u").child(0).data<std::uint8_t>()[0];
    CHECK((packed & 1) == 0);  // false first
    CHECK((packed & 2) != 0);  // then true
}

TEST_CASE("typed set_union gives an empty list for a group of nulls") {
    dstd::Vector<std::int64_t> v{1, 0};
    const std::uint8_t valid[1] = {0x01};  // row 1 is null
    Series c = Series::flat_i64(v.data(), 2, valid);
    DataFrame r = typed_of(c, {"a", "b"});
    REQUIRE(r.num_rows() == 2);
    CHECK(list_cell<std::int64_t>(r, 0) == dstd::Vector<std::int64_t>{1});
    CHECK(list_cell<std::int64_t>(r, 1).empty());
    CHECK_FALSE(r.column("u").is_null(1));
}

TEST_CASE("typed set_union over a list column lists the elements") {
    dstd::Vector<std::int64_t> elems{1, 2, 2, 3, 9};
    Series v = Series::list({0, 2, 4, 5}, Series::flat_i64(elems.data(), 5));
    DataFrame r = typed_of(v, {"a", "a", "b"});
    CHECK(list_cell<std::int64_t>(r, 0) == dstd::Vector<std::int64_t>{1, 2, 3});
    CHECK(list_cell<std::int64_t>(r, 1) == dstd::Vector<std::int64_t>{9});
}

TEST_CASE("the text form is unchanged and refuses the separator") {
    dstd::Vector<std::int64_t> v{10, 9};
    Series c = Series::flat_i64(v.data(), 2);
    DataFrame r = set_union_of(c, {"a", "a"});
    CHECK(r.column("u").type() == dataframe::TypeId::String);
    CHECK(cell(r, 0) == "10" + SEP + "9");
}

TEST_CASE("typed set_union merges with a serialized partial") {
    dstd::Vector<std::int64_t> k(400), v(400);
    for (std::int64_t i = 0; i < 400; ++i) {
        k[static_cast<std::size_t>(i)] = i % 2;
        v[static_cast<std::size_t>(i)] = (i * 7) % 23;
    }
    Series key = Series::flat_i64(k.data(), 400);
    Series val = Series::flat_i64(v.data(), 400);
    dstd::Vector<const Series*> vals{&val};
    dstd::Vector<dataframe::AggSpec> specs{
        {dataframe::AggOp::SetUnion, 0, "su", 1.0}};
    auto a = dataframe::agg_new(specs);
    dftu_utils_test::ok(dataframe::agg_accumulate(*a, key, vals, 0, 200));
    auto b = dataframe::agg_new(specs);
    dftu_utils_test::ok(dataframe::agg_accumulate(*b, key, vals, 200, 400));
    auto b2 = dftu_utils_test::ok(
        dataframe::agg_deserialize(dataframe::agg_serialize(*b)));
    dataframe::agg_merge(*a, *b2);
    DataFrame merged = dataframe::agg_finalize(*a, "k");
    auto full = dataframe::agg_new(specs);
    dftu_utils_test::ok(dataframe::agg_accumulate(*full, key, vals));
    DataFrame direct = dataframe::agg_finalize(*full, "k");
    REQUIRE(merged.num_rows() == direct.num_rows());
    for (std::int64_t r = 0; r < merged.num_rows(); ++r) {
        merged.names[1] = "u";
        direct.names[1] = "u";
        auto m = list_cell<std::int64_t>(merged, r);
        auto d = list_cell<std::int64_t>(direct, r);
        CHECK(m == d);
        CHECK(std::is_sorted(m.begin(), m.end()));
    }
}
