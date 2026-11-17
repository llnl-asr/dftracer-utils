// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dataframe/kernels/sort.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cmath>
#include <cstdint>

using namespace dftracer::utils;

namespace {

dataframe::Series ints(const dstd::Vector<std::int64_t>& v,
                       const dstd::Vector<std::uint8_t>& valid = {}) {
    dstd::Vector<std::uint8_t> bits((v.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < v.size(); ++i)
        if (valid.empty() || valid[i])
            bits[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
    return dataframe::Series::flat_i64(v.data(),
                                       static_cast<std::int64_t>(v.size()),
                                       valid.empty() ? nullptr : bits.data());
}

dstd::Vector<std::int64_t> values_of(const dataframe::Series& s) {
    const dataframe::Series m = s.materialize();
    dstd::Vector<std::int64_t> out;
    for (std::int64_t i = 0; i < m.length(); ++i)
        out.push_back(m.data<std::int64_t>()[i]);
    return out;
}

// Ties and nulls interleaved: rows 1, 4 and 6 are null; 0, 3 and 5 tie.
const dstd::Vector<std::int64_t> X = {5, 0, 2, 5, 0, 5, 0, 2};
const dstd::Vector<std::uint8_t> V = {1, 0, 1, 1, 0, 1, 0, 1};

}  // namespace

TEST_CASE("argsort keeps input order for ties and nulls in every mode") {
    const dataframe::Series s = ints(X, V);
    using I = dstd::Vector<std::int64_t>;
    CHECK(values_of(dataframe::argsort(s, false, true)) ==
          I{2, 7, 0, 3, 5, 1, 4, 6});
    CHECK(values_of(dataframe::argsort(s, false, false)) ==
          I{1, 4, 6, 2, 7, 0, 3, 5});
    CHECK(values_of(dataframe::argsort(s, true, true)) ==
          I{0, 3, 5, 2, 7, 1, 4, 6});
    CHECK(values_of(dataframe::argsort(s, true, false)) ==
          I{1, 4, 6, 0, 3, 5, 2, 7});
}

TEST_CASE("string argsort keeps input order for ties and nulls in every mode") {
    const dstd::StringView vals[] = {"b", "", "a", "b", "", "b", "", "a"};
    const std::uint8_t bits[] = {0b10101101};
    const dataframe::Series s = dataframe::Series::strings(
        dstd::Span<const dstd::StringView>(vals, 8), bits);
    using I = dstd::Vector<std::int64_t>;
    CHECK(values_of(dataframe::argsort(s, false, true)) ==
          I{2, 7, 0, 3, 5, 1, 4, 6});
    CHECK(values_of(dataframe::argsort(s, false, false)) ==
          I{1, 4, 6, 2, 7, 0, 3, 5});
    CHECK(values_of(dataframe::argsort(s, true, true)) ==
          I{0, 3, 5, 2, 7, 1, 4, 6});
    CHECK(values_of(dataframe::argsort(s, true, false)) ==
          I{1, 4, 6, 0, 3, 5, 2, 7});
}

TEST_CASE("sort_by_multi takes one nulls_last flag per key") {
    dataframe::DataFrame f;
    f.names = dstd::make_shared_strings({"a", "b", "row"});
    f.columns.push_back(ints(X, V));
    const dstd::Vector<std::int64_t> b = {1, 2, 3, 1, 2, 3, 1, 2};
    f.columns.push_back(ints(b));
    const dstd::Vector<std::int64_t> row = {0, 1, 2, 3, 4, 5, 6, 7};
    f.columns.push_back(ints(row));
    using I = dstd::Vector<std::int64_t>;
    auto out = dftu_utils_test::ok(
        f.sort_by_multi({"a", "b"}, dstd::Vector<std::uint8_t>{0, 0},
                        dstd::Vector<std::uint8_t>{0, 1}));
    CHECK(values_of(out.column("row")) == I{6, 1, 4, 7, 2, 0, 3, 5});
    out = dftu_utils_test::ok(
        f.sort_by_multi({"a", "b"}, dstd::Vector<std::uint8_t>{1, 1},
                        dstd::Vector<std::uint8_t>{0, 0}));
    CHECK(values_of(out.column("row")) == I{1, 4, 6, 5, 0, 3, 2, 7});
}

TEST_CASE("a lazy sort with nulls first matches the eager one") {
    dataframe::DataFrame f;
    f.names = dstd::make_shared_strings({"a", "row"});
    const std::int64_t n = 3000;
    dstd::Vector<std::int64_t> a(static_cast<std::size_t>(n)), row(a.size());
    dstd::Vector<std::uint8_t> valid(a.size());
    for (std::int64_t i = 0; i < n; ++i) {
        a[static_cast<std::size_t>(i)] = (i * 7919) % 23;
        row[static_cast<std::size_t>(i)] = i;
        valid[static_cast<std::size_t>(i)] = i % 5 != 0;
    }
    f.columns.push_back(ints(a, valid));
    f.columns.push_back(ints(row));
    for (const bool desc : {false, true}) {
        const auto eager = dftu_utils_test::ok(f.sort_by("a", desc, false));
        const auto lazy =
            dftu_utils_test::ok(dftracer::utils::default_runtime()
                                    .submit(f.lazy()
                                                .memory_budget(4096)
                                                .sort_by("a", desc, false)
                                                .collect(64))
                                    .get());
        CHECK(values_of(lazy.column("row")) == values_of(eager.column("row")));
    }
}

TEST_CASE("unique keeps one null and can keep first-occurrence order") {
    const dataframe::Series s = ints({3, 0, 1, 3, 0, 1}, {1, 0, 1, 1, 0, 1});
    CHECK(s.unique(true).length() == 3);
    CHECK(values_of(s.unique(true).drop_nulls()) ==
          dstd::Vector<std::int64_t>{3, 1});
    CHECK(s.unique(true).is_null(1));
    const dataframe::Series sorted = s.unique(false);
    CHECK(sorted.length() == 3);
    CHECK(sorted.is_null(0));
    CHECK(values_of(sorted.drop_nulls()) == dstd::Vector<std::int64_t>{1, 3});
    CHECK(s.nunique() == 2);
}

TEST_CASE("search_sorted right inserts after equal values") {
    const dataframe::Series s = ints({1, 2, 2, 2, 5});
    const dataframe::Series q = ints({2, 0, 6});
    CHECK(values_of(s.search_sorted(q, false)) ==
          dstd::Vector<std::int64_t>{1, 0, 5});
    CHECK(values_of(s.search_sorted(q, true)) ==
          dstd::Vector<std::int64_t>{4, 0, 5});
}

TEST_CASE("duplicated marks the repeats by the occurrence kept") {
    const dataframe::Series s = ints({1, 2, 1, 1, 2, 3});
    const auto flags = [&](std::int32_t keep) {
        const dataframe::Series m = s.duplicated(keep).materialize();
        dstd::Vector<int> out;
        for (std::int64_t i = 0; i < m.length(); ++i)
            out.push_back((m.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1);
        return out;
    };
    CHECK(flags(0) == dstd::Vector<int>{0, 0, 1, 1, 1, 0});
    CHECK(flags(1) == dstd::Vector<int>{1, 1, 1, 0, 0, 0});
    CHECK(flags(2) == dstd::Vector<int>{1, 1, 1, 1, 1, 0});
}

TEST_CASE("the unbiased skew and kurtosis follow the pandas estimators") {
    dataframe::FieldStat fs;
    for (double x : {1.0, 2.0, 4.0, 8.0, 9.0}) fs.add(x);
    // pandas Series([1, 2, 4, 8, 9]).skew() and .kurt()
    CHECK(fs.skewness_unbiased() == doctest::Approx(0.2717687582352622));
    CHECK(fs.kurtosis_unbiased() == doctest::Approx(-2.6802653605307194));
    dataframe::FieldStat two;
    two.add(1.0);
    two.add(2.0);
    two.add(3.0);
    CHECK(std::isnan(two.kurtosis_unbiased()));
    CHECK(two.skewness_unbiased() == doctest::Approx(0.0));
    dataframe::FieldStat flat;
    for (int i = 0; i < 5; ++i) flat.add(2.0);
    CHECK(std::isnan(flat.skewness_unbiased()));
    // Merging two halves gives the one-pass value.
    dataframe::FieldStat a, b;
    for (double x : {1.0, 2.0}) a.add(x);
    for (double x : {4.0, 8.0, 9.0}) b.add(x);
    a.merge(b);
    CHECK(a.skewness_unbiased() == doctest::Approx(fs.skewness_unbiased()));
    CHECK(a.kurtosis_unbiased() == doctest::Approx(fs.kurtosis_unbiased()));
}

TEST_CASE("the unbiased moments survive a serialized partial and a merge") {
    const dstd::Vector<double> x = {1.0, 2.0, 4.0, 8.0, 9.0, 3.0, 7.0};
    const dstd::Vector<std::int64_t> keys(x.size(), 1);
    auto part = [&](std::size_t from, std::size_t to) {
        const auto n = static_cast<std::int64_t>(to - from);
        const dataframe::Series k = dataframe::Series::flat_i64(keys.data(), n);
        const dataframe::Series v = dataframe::Series::flat_f64(&x[from], n);
        auto st = dataframe::agg_new(
            {dataframe::AggSpec{dataframe::AggOp::SkewUnbiased, 0, "s"},
             dataframe::AggSpec{dataframe::AggOp::KurtUnbiased, 0, "k"}});
        dftu_utils_test::ok(dataframe::agg_accumulate(*st, {&k}, {&v}));
        return st;
    };
    auto whole = part(0, x.size());
    auto left = part(0, 3);
    auto right = dftu_utils_test::ok(dataframe::agg_deserialize(
        dataframe::agg_serialize(*part(3, x.size()))));
    dataframe::agg_merge(*left, *right);
    const dstd::Vector<dstd::String> names{"g"};
    const dataframe::DataFrame a = dataframe::agg_finalize(*whole, names);
    const dataframe::DataFrame b = dataframe::agg_finalize(*left, names);
    for (const char* name : {"s", "k"})
        CHECK(b.column(name).data<double>()[0] ==
              doctest::Approx(a.column(name).data<double>()[0]));
    dataframe::FieldStat fs;
    for (double d : x) fs.add(d);
    CHECK(a.column("s").data<double>()[0] ==
          doctest::Approx(fs.skewness_unbiased()));
    CHECK(a.column("k").data<double>()[0] ==
          doctest::Approx(fs.kurtosis_unbiased()));
}

TEST_CASE("eager to_dummies and pivot give a null key no column and no row") {
    dataframe::DataFrame f;
    f.names = dstd::make_shared_strings({"k", "r", "v"});
    f.columns.push_back(ints({7, 0, 7, 8, 8}, {1, 0, 1, 1, 1}));
    f.columns.push_back(ints({1, 1, 0, 2, 2}, {1, 1, 0, 1, 1}));
    f.columns.push_back(ints({10, 20, 30, 40, 50}));
    const dataframe::DataFrame dummies = dftu_utils_test::ok(f.to_dummies("k"));
    CHECK(dummies.columns.size() == 4);
    const dataframe::DataFrame wide =
        dftu_utils_test::ok(f.pivot("r", "k", "v", "sum"));
    CHECK(wide.columns.size() == 3);
    CHECK(wide.num_rows() == 2);
}

TEST_CASE("sort_by_multi flag spans: empty is the default, sizes are checked") {
    dataframe::DataFrame f;
    f.names = dstd::make_shared_strings({"a", "row"});
    f.columns.push_back(ints(X, V));
    const dstd::Vector<std::int64_t> row = {0, 1, 2, 3, 4, 5, 6, 7};
    f.columns.push_back(ints(row));
    using I = dstd::Vector<std::int64_t>;
    const dstd::Span<const std::uint8_t> none;
    auto out = dftu_utils_test::ok(f.sort_by_multi({"a"}, none, none));
    CHECK(values_of(out.column("row")) == I{2, 7, 0, 3, 5, 1, 4, 6});
    const dstd::Vector<std::uint8_t> three = {0, 0, 0};
    CHECK(!f.sort_by_multi({"a"}, three, none).has_value());
    CHECK(!f.sort_by_multi({"a"}, none, three).has_value());
}

TEST_CASE("the per-column C sort refuses null arrays and bad sizes") {
    const std::int64_t a[] = {5, 0, 2, 5};
    const std::uint8_t valid[] = {0b1101};
    const std::int64_t b[] = {1, 2, 3, 1};
    const char* names[] = {"a", "b"};
    dftu_series* cols[] = {
        dftu_series_new_flat(DFTU_TYPE_INT64, a, 4, valid),
        dftu_series_new_flat(DFTU_TYPE_INT64, b, 4, nullptr)};
    dftu_dataframe* h = dftu_dataframe_new(names, cols, 2);
    REQUIRE(h != nullptr);
    const std::int32_t one[] = {0};
    const std::int32_t two[] = {0, 1};
    const std::int32_t three[] = {0, 0, 0};
    CHECK(dftu_dataframe_sort_by_multi_per_col_nulls(h, names, 2, nullptr, 1,
                                                     one, 1) == nullptr);
    CHECK(dftu_dataframe_sort_by_multi_per_col_nulls(h, names, 2, one, 1,
                                                     nullptr, 1) == nullptr);
    CHECK(dftu_dataframe_sort_by_multi_per_col_nulls(h, nullptr, 2, one, 1, one,
                                                     1) == nullptr);
    CHECK(dftu_dataframe_sort_by_multi_per_col_nulls(h, names, 2, one, -1, one,
                                                     1) == nullptr);
    CHECK(dftu_dataframe_sort_by_multi_per_col_nulls(h, names, 2, three, 3, one,
                                                     1) == nullptr);
    CHECK(dftu_dataframe_sort_by_multi_nulls(h, nullptr, 2, 0, 1) == nullptr);
    CHECK(dftu_dataframe_sort_by_nulls(h, nullptr, 0, 1) == nullptr);
    dftu_dataframe* sorted =
        dftu_dataframe_sort_by_multi_per_col_nulls(h, names, 2, two, 2, one, 1);
    CHECK(sorted != nullptr);
    dftu_dataframe_free(sorted);
    dftu_dataframe_free(h);
}
