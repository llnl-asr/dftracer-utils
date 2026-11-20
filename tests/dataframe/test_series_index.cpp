// SPDX-License-Identifier: MIT
// The row labels of a Series: kept with the rows by the ops that keep or move
// them, shared by copies, dropped by reducers and aligned between two columns.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/kernels/cast.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

Series i64s(dstd::Vector<std::int64_t> v) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
}

Series f64s(dstd::Vector<double> v) {
    return Series::flat_f64(v.data(), static_cast<std::int64_t>(v.size()));
}

Series labelled(Series values, Series level, dstd::StringView name = "k") {
    const dstd::StringView names[] = {name};
    Series levels[] = {std::move(level)};
    return values.with_index(names, levels);
}

dstd::Vector<std::int64_t> ints(const Series& s) {
    dstd::Vector<std::int64_t> out;
    for (const std::int64_t v : s.values<std::int64_t>()) out.push_back(v);
    return out;
}

dstd::Vector<double> doubles(const Series& s) {
    dstd::Vector<double> out;
    for (const double v : s.values<double>()) out.push_back(v);
    return out;
}

dstd::Vector<std::int64_t> labels(const Series& s, std::int32_t level = 0) {
    return ints(s.index_level(level));
}

// v = [3, 1, 2, 2] under the labels k = [30, 10, 20, 40].
Series sample() {
    return labelled(f64s({3.0, 1.0, 2.0, 2.0}), i64s({30, 10, 20, 40}));
}

}  // namespace

TEST_CASE("a column has no labels until it is given some") {
    Series s = f64s({1.0, 2.0});
    CHECK(s.index_count() == 0);
    CHECK_FALSE(s.has_index());
    CHECK(s.index_frame().num_columns() == 0);
    CHECK_FALSE(s.index_level(0).valid());
}

TEST_CASE("with_index checks the levels") {
    const Series s = f64s({1.0, 2.0});
    const dstd::StringView one[] = {"k"};
    const dstd::StringView two[] = {"k", "k"};
    Series short_level[] = {i64s({1})};
    CHECK_FALSE(s.with_index(one, short_level).valid());
    Series twice[] = {i64s({1, 2}), i64s({1, 2})};
    CHECK_FALSE(s.with_index(two, twice).valid());
    Series fine[] = {i64s({5, 6})};
    const Series t = s.with_index(one, fine);
    REQUIRE(t.valid());
    CHECK(t.index_count() == 1);
    CHECK(t.index_name(0) == "k");
    CHECK(labels(t) == dstd::Vector<std::int64_t>{5, 6});
    CHECK_FALSE(t.without_index().has_index());
}

TEST_CASE("the C ABI reads and sets labels") {
    const Series s = sample();
    CHECK(dftu_series_index_count(s.handle()) == 1);
    const char* name = nullptr;
    std::size_t len = 0;
    CHECK(dftu_series_index_name(s.handle(), 0, &name, &len) == 0);
    CHECK(dstd::StringView(name, len) == "k");
    CHECK(dftu_series_index_name(s.handle(), 1, &name, &len) == -1);
    dftu_series* level = dftu_series_index_column(s.handle(), 0);
    REQUIRE(level != nullptr);
    CHECK(dftu_series_index_count(level) == 0);
    dftu_series_free(level);
    CHECK(dftu_series_index_column(s.handle(), 3) == nullptr);
    CHECK(dftu_series_index_count(nullptr) == 0);
    CHECK(dftu_series_with_index(nullptr, nullptr, nullptr, nullptr, 0) ==
          nullptr);
}

TEST_CASE("share, rename and slice keep the labels") {
    const Series s = sample();
    CHECK(s.share().has_index());
    CHECK(s.rename("z").has_index());
    const Series part = s.slice(1, 2);
    REQUIRE(part.has_index());
    CHECK(labels(part) == dstd::Vector<std::int64_t>{10, 20});
    CHECK(part.length() == part.index_level(0).length());
}

TEST_CASE("an op that keeps the rows keeps the labels") {
    const Series s = sample();
    const Series counts = labelled(i64s({1, 2, 3}), i64s({7, 8, 9}));
    CHECK(labels(counts.cumsum()) == (dstd::Vector<std::int64_t>{7, 8, 9}));
    const auto keeps = [](dstd::StringView op, const Series& out) {
        INFO(doctest::String(op.data(), static_cast<unsigned>(op.size())));
        REQUIRE(out.valid());
        CHECK(out.index_name(0) == "k");
        CHECK(labels(out) == dstd::Vector<std::int64_t>{30, 10, 20, 40});
    };
    keeps("shift", s.shift(1));
    keeps("diff", s.diff());
    keeps("abs", s.abs());
    keeps("negate", s.negate());
    keeps("add scalar", s + 1.5);
    keeps("mul scalar", s * 2.0);
    keeps("fillna", s.fillna(0.0));
    keeps("cast", s.cast(TypeId::Float32));
}

TEST_CASE("an op that moves rows moves the labels with them") {
    const Series s = sample();
    const Series sorted = s.sort();
    CHECK(doubles(sorted) == dstd::Vector<double>{1.0, 2.0, 2.0, 3.0});
    CHECK(labels(sorted)[0] == 10);
    CHECK(labels(sorted)[3] == 30);
    CHECK(labels(s.sort(true)) == (dstd::Vector<std::int64_t>{30, 20, 40, 10}));
    CHECK(labels(s.head(2)) == dstd::Vector<std::int64_t>{30, 10});
    CHECK(labels(s.tail(2)) == dstd::Vector<std::int64_t>{20, 40});
    CHECK(labels(s.reverse()) == (dstd::Vector<std::int64_t>{40, 20, 10, 30}));
    const std::int64_t at[] = {3, 0};
    CHECK(labels(s.take(at)) == dstd::Vector<std::int64_t>{40, 30});
    const std::int64_t bad[] = {4};
    CHECK_FALSE(s.take(bad).valid());
    const Series mask = s.gt(1.5);
    const Series kept = s.filter(mask);
    CHECK(doubles(kept) == dstd::Vector<double>{3.0, 2.0, 2.0});
    CHECK(labels(kept) == (dstd::Vector<std::int64_t>{30, 20, 40}));
    CHECK(labels(s.top_k(1)) == dstd::Vector<std::int64_t>{30});
    CHECK(labels(s.bottom_k(1)) == dstd::Vector<std::int64_t>{10});
    CHECK(s.sample(2, 7).index_level(0).length() == 2);
}

TEST_CASE("drop_nulls drops the labels of the null rows only") {
    const std::uint8_t valid[] = {0b1101};
    Series values = Series::flat_f64(
        dstd::Vector<double>{1.0, 0.0, 3.0, 4.0}.data(), 4, valid);
    const Series s = labelled(std::move(values), i64s({5, 6, 7, 8}));
    const Series out = s.drop_nulls();
    CHECK(out.length() == 3);
    CHECK(labels(out) == dstd::Vector<std::int64_t>{5, 7, 8});
}

TEST_CASE("unique and a reducer give no labels") {
    const Series s = sample();
    CHECK_FALSE(s.unique().has_index());
    CHECK_FALSE(f64s({1.0, 2.0}).unique().has_index());
    Series all_distinct =
        labelled(f64s({3.0, 1.0, 2.0}), i64s({1, 2, 3}), "k").unique();
    CHECK_FALSE(all_distinct.has_index());
}

TEST_CASE("the labels of two columns that match are kept in place") {
    const Series a = sample();
    const Series b =
        labelled(f64s({10.0, 20.0, 30.0, 40.0}), i64s({30, 10, 20, 40}));
    const Series sum = a + b;
    CHECK(doubles(sum) == dstd::Vector<double>{13.0, 21.0, 32.0, 42.0});
    CHECK(labels(sum) == (dstd::Vector<std::int64_t>{30, 10, 20, 40}));
    CHECK(same_labels(a, b));
}

TEST_CASE("two columns with other labels align on their union, sorted") {
    const Series a = sample();
    const Series b = labelled(f64s({1.0, 2.0, 3.0}), i64s({10, 20, 50}));
    CHECK_FALSE(same_labels(a, b));
    const Series sum = a + b;
    REQUIRE(sum.valid());
    CHECK(labels(sum) == (dstd::Vector<std::int64_t>{10, 20, 30, 40, 50}));
    CHECK(sum.null_count() == 3);
    CHECK(sum.is_null(2));
    CHECK_FALSE(sum.is_null(0));
    CHECK(doubles(sum)[0] == 2.0);
    CHECK(doubles(sum)[1] == 4.0);
}

TEST_CASE("an integer column that gains a missing row widens to Float64") {
    const Series a = labelled(i64s({1, 2}), i64s({1, 2}));
    const Series b = labelled(i64s({10, 20}), i64s({2, 3}));
    const Series sum = a + b;
    CHECK(sum.type() == TypeId::Float64);
    CHECK(labels(sum) == (dstd::Vector<std::int64_t>{1, 2, 3}));
    CHECK(sum.null_count() == 2);
    const Series same = a + labelled(i64s({5, 6}), i64s({1, 2}));
    CHECK(same.type() == TypeId::Int64);
}

TEST_CASE("duplicated labels match every row that has them") {
    const Series a = labelled(f64s({1.0, 2.0, 3.0}), i64s({1, 1, 2}));
    const Series b = labelled(f64s({10.0, 20.0, 30.0}), i64s({1, 2, 2}));
    const Series sum = a + b;
    REQUIRE(sum.valid());
    CHECK(labels(sum) == (dstd::Vector<std::int64_t>{1, 1, 2, 2}));
    CHECK(doubles(sum) == (dstd::Vector<double>{11.0, 12.0, 23.0, 33.0}));
}

TEST_CASE("a positional column counts as the labels 0 .. n-1") {
    const Series a = labelled(f64s({1.0, 2.0, 3.0}), i64s({0, 1, 2}));
    const Series p = f64s({10.0, 20.0, 30.0});
    CHECK(same_labels(a, p));
    const Series sum = a + p;
    CHECK(doubles(sum) == (dstd::Vector<double>{11.0, 22.0, 33.0}));
    const Series shifted = labelled(f64s({1.0, 2.0, 3.0}), i64s({1, 2, 3}));
    const Series other = shifted + p;
    REQUIRE(other.valid());
    CHECK(labels(other) == (dstd::Vector<std::int64_t>{0, 1, 2, 3}));
    CHECK(other.null_count() == 2);
    CHECK_FALSE((p + p).has_index());
}

TEST_CASE("labels that cannot align are an error") {
    const Series a = sample();
    Series text = Series::strings(dstd::Vector<dstd::StringView>{"a", "b"});
    const Series b = labelled(f64s({1.0, 2.0}), std::move(text));
    CHECK_FALSE(align_labels(a, b).has_value());
    Series two[] = {i64s({1, 2}), i64s({3, 4})};
    const dstd::StringView names[] = {"x", "y"};
    const Series multi = f64s({1.0, 2.0}).with_index(names, two);
    CHECK_FALSE(align_labels(b, multi).has_value());
}

TEST_CASE("a two-level index aligns level by level") {
    Series g = Series::strings(dstd::Vector<dstd::StringView>{"a", "a", "b"});
    Series n = i64s({1, 2, 1});
    const dstd::StringView names[] = {"g", "n"};
    Series la[] = {std::move(g), std::move(n)};
    const Series a = f64s({1.0, 2.0, 3.0}).with_index(names, la);
    Series g2 = Series::strings(dstd::Vector<dstd::StringView>{"a", "c"});
    Series n2 = i64s({2, 1});
    Series lb[] = {std::move(g2), std::move(n2)};
    const Series b = f64s({10.0, 20.0}).with_index(names, lb);
    const Series sum = a + b;
    REQUIRE(sum.valid());
    CHECK(sum.index_count() == 2);
    CHECK(sum.length() == 4);
    CHECK(sum.index_name(0) == "g");
    CHECK(sum.null_count() == 3);
}

TEST_CASE("a null label equals a null label in the same row") {
    const std::uint8_t valid[] = {0b101};
    const std::int64_t raw[] = {7, 0, 9};
    const Series la = Series::flat_i64(raw, 3, valid);
    const Series lb = Series::flat_i64(raw, 3, valid);
    const Series a = labelled(f64s({1.0, 2.0, 3.0}), la.share());
    const Series b = labelled(f64s({10.0, 20.0, 30.0}), lb.share());
    CHECK(same_labels(a, b));
    const Series sum = a + b;
    CHECK(doubles(sum) == (dstd::Vector<double>{11.0, 22.0, 33.0}));
    CHECK(sum.index_level(0).null_count() == 1);
    const std::uint8_t other[] = {0b110};
    const Series moved = Series::flat_i64(raw, 3, other);
    CHECK_FALSE(same_labels(a, labelled(f64s({1.0, 2.0, 3.0}), moved.share())));
}

TEST_CASE("a null label meets a null label of the other side") {
    const std::uint8_t first[] = {0b01};
    const std::uint8_t second[] = {0b10};
    const std::int64_t raw[] = {1, 2};
    const Series a =
        labelled(f64s({1.0, 2.0}), Series::flat_i64(raw, 2, first));
    const Series b =
        labelled(f64s({10.0, 20.0}), Series::flat_i64(raw, 2, second));
    const Series sum = a + b;
    REQUIRE(sum.valid());
    CHECK(sum.length() == 3);
    CHECK(sum.index_level(0).null_count() == 1);
    CHECK(sum.null_count() == 2);
    CHECK(doubles(sum)[2] == 12.0);
}

TEST_CASE("numeric labels of different types meet in a common type") {
    const Series narrow = cast(i64s({2, 1, 0}), TypeId::Int32);
    const Series a = labelled(f64s({1.0, 2.0, 3.0}), narrow.share());
    const Series p = f64s({10.0, 20.0, 30.0});
    CHECK(same_labels(a, p.without_index()) == false);
    const Series sum = a + p;
    REQUIRE(sum.valid());
    CHECK(sum.index_level(0).type() == TypeId::Int64);
    CHECK(doubles(sum) == (dstd::Vector<double>{13.0, 22.0, 31.0}));
    const Series real = labelled(f64s({1.0, 2.0}), f64s({1.0, 3.0}));
    const Series mixed = labelled(f64s({1.0, 2.0}), i64s({1, 2})) + real;
    REQUIRE(mixed.valid());
    CHECK(mixed.index_level(0).type() == TypeId::Float64);
    CHECK(mixed.length() == 3);
    const Series wide = cast(i64s({1, 2}), TypeId::Uint8);
    CHECK(align_labels(labelled(f64s({1.0, 2.0}), wide.share()), real)
              .has_value());
}

TEST_CASE("a Bool column keeps its type and takes false for a missing row") {
    const std::uint8_t first[] = {0b101};
    const std::uint8_t second[] = {0b11};
    const Series a =
        labelled(Series::flat(TypeId::Bool, first, 3), i64s({1, 2, 3}));
    const Series b =
        labelled(Series::flat(TypeId::Bool, second, 2), i64s({2, 3}));
    const Series both = a & b;
    REQUIRE(both.valid());
    CHECK(both.type() == TypeId::Bool);
    CHECK(both.length() == 3);
    CHECK(both.null_count() == 0);
    const Series either = a | b;
    REQUIRE(either.valid());
    CHECK(either.type() == TypeId::Bool);
}

TEST_CASE("where and str_cat need the same labels") {
    const Series a = sample();
    const Series other =
        labelled(f64s({0.0, 0.0, 0.0, 0.0}), i64s({1, 2, 3, 4}));
    const std::uint8_t bits[] = {0b0101};
    const Series mask = Series::flat(TypeId::Bool, bits, 4);
    CHECK_FALSE(a.where(mask, other).valid());
    const Series same =
        labelled(f64s({0.0, 0.0, 0.0, 0.0}), i64s({30, 10, 20, 40}));
    const Series picked = a.where(mask, same);
    REQUIRE(picked.valid());
    CHECK(labels(picked) == (dstd::Vector<std::int64_t>{30, 10, 20, 40}));
    const Series unlabelled = f64s({0.0, 0.0, 0.0, 0.0});
    const Series from_other = unlabelled.where(mask, a);
    REQUIRE(from_other.valid());
    CHECK(labels(from_other) == (dstd::Vector<std::int64_t>{30, 10, 20, 40}));
    const Series x =
        labelled(Series::strings(dstd::Vector<dstd::StringView>{"a", "b"}),
                 i64s({1, 2}));
    const Series y =
        labelled(Series::strings(dstd::Vector<dstd::StringView>{"c", "d"}),
                 i64s({2, 3}));
    CHECK_FALSE(x.str_cat(y).valid());
    CHECK(x.str_cat(x).valid());
}

TEST_CASE("a Bool column keeps a null it already had when labels align") {
    const std::uint8_t bits[] = {0b101};
    const std::uint8_t valid[] = {0b101};
    const Series a =
        labelled(Series::flat(TypeId::Bool, bits, 3, valid), i64s({1, 2, 3}));
    const std::uint8_t ones[] = {0b111};
    const Series b =
        labelled(Series::flat(TypeId::Bool, ones, 3), i64s({2, 3, 4}));
    const Series both = a & b;
    REQUIRE(both.valid());
    CHECK(both.length() == 4);
    CHECK(both.null_count() == 1);
    CHECK(ints(cast(both.null_mask(), TypeId::Int64)) ==
          (dstd::Vector<std::int64_t>{0, 1, 0, 0}));
}
