// SPDX-License-Identifier: MIT
// Reindex and align by row label: Series::reindex, DataFrame::reindex,
// align_labels(how) and align_rows over the label index.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

using Ints = dstd::Vector<std::int64_t>;

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

Ints ints(const Series& s) {
    Ints out;
    for (const std::int64_t v : s.values<std::int64_t>()) out.push_back(v);
    return out;
}

Ints labels(const Series& s, std::int32_t level = 0) {
    return ints(s.index_level(level));
}

Series reindexed(const Series& s, Series keys, const ReindexOptions& o = {}) {
    Series list[] = {std::move(keys)};
    auto r = s.reindex(list, o);
    REQUIRE(r.has_value());
    return std::move(*r);
}

dstd::String error_of(const Series& s, Series keys,
                      const ReindexOptions& o = {}) {
    Series list[] = {std::move(keys)};
    auto r = s.reindex(list, o);
    REQUIRE_FALSE(r.has_value());
    return dstd::String(r.error().c_str());
}

// v = [1, 2, 3] under k = [10, 20, 30].
Series ladder() { return labelled(i64s({1, 2, 3}), i64s({10, 20, 30})); }

}  // namespace

TEST_CASE("a label with no row gets a null and an integer column widens") {
    const Series r = reindexed(ladder(), i64s({20, 40, 10}));
    CHECK(r.type() == TypeId::Float64);
    CHECK(labels(r) == Ints{20, 40, 10});
    CHECK(r.index_name(0) == "k");
    CHECK(r.values<double>()[0] == 2.0);
    CHECK(r.is_null(1));
    CHECK(r.values<double>()[2] == 1.0);
}

TEST_CASE("a reindex that misses nothing keeps the type") {
    const Series r = reindexed(ladder(), i64s({30, 10}));
    CHECK(r.type() == TypeId::Int64);
    CHECK(ints(r) == Ints{3, 1});
    CHECK(r.null_count() == 0);
}

TEST_CASE("a fill value keeps the type when it fits") {
    Series keys[] = {i64s({20, 40})};
    Series zero[] = {i64s({0})};
    auto r = ladder().reindex(keys, {}, zero[0]);
    REQUIRE(r.has_value());
    CHECK(r->type() == TypeId::Int64);
    CHECK(ints(*r) == Ints{2, 0});

    Series half[] = {f64s({1.5})};
    auto w = ladder().reindex(keys, {}, half[0]);
    REQUIRE(w.has_value());
    CHECK(w->type() == TypeId::Float64);
    CHECK(w->values<double>()[1] == 1.5);

    const dstd::String word("x");
    const dstd::StringView views[] = {dstd::StringView(word)};
    const Series text = Series::strings(views);
    auto bad = ladder().reindex(keys, {}, text);
    CHECK_FALSE(bad.has_value());
}

TEST_CASE("a fill value leaves a null the column already held") {
    const std::uint8_t valid[] = {0b101};
    const std::int64_t v[] = {1, 2, 3};
    const Series s =
        labelled(Series::flat_i64(v, 3, valid), i64s({10, 20, 30}));
    Series keys[] = {i64s({20, 99})};
    Series fill[] = {i64s({7})};
    auto r = s.reindex(keys, {}, fill[0]);
    REQUIRE(r.has_value());
    CHECK(r->is_null(0));
    CHECK(r->values<std::int64_t>()[1] == 7);
}

TEST_CASE("keys of another type give the keys' own labels") {
    const Series r = reindexed(ladder(), f64s({10.0, 20.5}));
    CHECK(r.index_level(0).type() == TypeId::Float64);
    CHECK(r.values<double>()[0] == 1.0);
    CHECK(r.is_null(1));
}

TEST_CASE("repeated labels refuse a reindex unless the keys are the labels") {
    const Series dup = labelled(i64s({1, 2, 3}), i64s({1, 1, 2}));
    CHECK(error_of(dup, i64s({1, 2})) ==
          "cannot reindex on an axis with duplicate labels");
    const Series same = reindexed(dup, i64s({1, 1, 2}));
    CHECK(ints(same) == Ints{1, 2, 3});
    // Repeated keys over unique labels repeat the rows.
    CHECK(ints(reindexed(ladder(), i64s({10, 10, 20}))) == Ints{1, 1, 2});
}

TEST_CASE("a positional column reindexes by position") {
    const Series s = i64s({5, 6, 7});
    const Series r = reindexed(s, i64s({2, 0, 5}));
    CHECK(r.is_null(2));
    CHECK(r.values<double>()[0] == 7.0);
    CHECK(r.index_name(0) == "__index_level_0__");
    CHECK(labels(r) == Ints{2, 0, 5});
}

TEST_CASE("a MultiIndex reindexes by full tuples") {
    const dstd::StringView names[] = {"a", "b"};
    const dstd::StringView words[] = {"x", "y", "x"};
    Series levels[] = {i64s({1, 1, 2}), Series::strings(words)};
    const Series s = i64s({10, 20, 30}).with_index(names, levels);
    const dstd::StringView kw[] = {"x", "z", "y"};
    Series keys[] = {i64s({2, 3, 1}), Series::strings(kw)};
    auto r = s.reindex(keys);
    REQUIRE(r.has_value());
    CHECK(r->values<double>()[0] == 30.0);
    CHECK(r->is_null(1));
    CHECK(r->values<double>()[2] == 20.0);
    CHECK(r->index_name(1) == "b");
    Series partial[] = {i64s({1})};
    CHECK_FALSE(s.reindex(partial).has_value());
}

TEST_CASE("pad, backfill and nearest fill from monotonic labels") {
    ReindexOptions pad;
    pad.method = FillMethod::Pad;
    const Series p = reindexed(ladder(), i64s({5, 10, 15, 25, 35, 40}), pad);
    CHECK(p.is_null(0));
    CHECK(p.values<double>()[1] == 1.0);
    CHECK(p.values<double>()[2] == 1.0);
    CHECK(p.values<double>()[3] == 2.0);
    CHECK(p.values<double>()[4] == 3.0);

    ReindexOptions back;
    back.method = FillMethod::Backfill;
    const Series b = reindexed(ladder(), i64s({5, 15, 25, 35}), back);
    CHECK(b.values<double>()[0] == 1.0);
    CHECK(b.values<double>()[1] == 2.0);
    CHECK(b.is_null(3));

    ReindexOptions near;
    near.method = FillMethod::Nearest;
    const Series n = reindexed(ladder(), i64s({5, 14, 16, 25, 40}), near);
    CHECK(ints(n) == Ints{1, 1, 2, 3, 3});
}

TEST_CASE("decreasing labels fill in label order") {
    const Series s = labelled(i64s({1, 2, 3}), i64s({30, 20, 10}));
    ReindexOptions pad;
    pad.method = FillMethod::Pad;
    const Series p = reindexed(s, i64s({5, 15, 25, 35}), pad);
    CHECK(p.values<double>()[0] == 3.0);
    CHECK(p.values<double>()[1] == 2.0);
    CHECK(p.values<double>()[2] == 1.0);
    CHECK(p.is_null(3));
}

TEST_CASE(
    "limit counts the labels a row fills and tolerance bounds the reach") {
    ReindexOptions o;
    o.method = FillMethod::Pad;
    o.limit = 1;
    const Series l = reindexed(ladder(), i64s({10, 11, 12, 20, 21, 22}), o);
    CHECK(l.values<double>()[1] == 1.0);
    CHECK(l.is_null(2));
    CHECK(l.values<double>()[4] == 2.0);
    CHECK(l.is_null(5));

    ReindexOptions t;
    t.method = FillMethod::Nearest;
    t.tolerance = 4;
    const Series n = reindexed(ladder(), i64s({5, 14, 16, 25}), t);
    CHECK(n.is_null(0));
    CHECK(n.values<double>()[1] == 1.0);
    CHECK(n.values<double>()[2] == 2.0);
    CHECK(n.is_null(3));
}

TEST_CASE("fill options are refused where pandas refuses them") {
    ReindexOptions unsorted;
    unsorted.method = FillMethod::Pad;
    const Series jumbled = labelled(i64s({1, 2, 3}), i64s({10, 30, 20}));
    CHECK(error_of(jumbled, i64s({5}), unsorted) ==
          "index must be monotonic increasing or decreasing");
    ReindexOptions limit;
    limit.limit = 1;
    CHECK(error_of(ladder(), i64s({5}), limit) ==
          "limit argument only valid if doing pad, backfill or nearest "
          "reindexing");
    ReindexOptions tol;
    tol.tolerance = 1;
    CHECK(error_of(ladder(), i64s({5}), tol) ==
          "tolerance argument only valid if doing pad, backfill or nearest "
          "reindexing");
    ReindexOptions scattered;
    scattered.method = FillMethod::Pad;
    scattered.limit = 1;
    CHECK(error_of(ladder(), i64s({25, 5}), scattered) ==
          "limit argument for 'pad' method only well-defined if index and "
          "target are monotonic");
    const dstd::StringView words[] = {"b", "d", "f"};
    Series text_levels[] = {Series::strings(words)};
    const dstd::StringView k[] = {"k"};
    const Series text = i64s({1, 2, 3}).with_index(k, text_levels);
    const dstd::StringView want[] = {"c", "e"};
    ReindexOptions nearest;
    nearest.method = FillMethod::Nearest;
    CHECK_FALSE(error_of(text, Series::strings(want), nearest).empty());
    const Series filled = reindexed(text, Series::strings(want), unsorted);
    CHECK(ints(filled) == Ints{1, 2});
}

TEST_CASE("a frame reindexes its columns together under the keys") {
    DataFrame f;
    f.names.emplace_back("a");
    f.columns.push_back(i64s({1, 2, 3}));
    f.names.emplace_back("b");
    f.columns.push_back(f64s({1.5, 2.5, 3.5}));
    f.names.emplace_back("k");
    f.columns.push_back(i64s({10, 20, 30}));
    const dstd::StringView idx[] = {"k"};
    auto indexed = f.with_index(idx);
    REQUIRE(indexed.has_value());
    Series keys[] = {i64s({30, 99})};
    auto r = indexed->reindex(keys);
    REQUIRE(r.has_value());
    CHECK(r->num_rows() == 2);
    CHECK(r->index_names().size() == 1);
    CHECK(ints(r->column("k")) == Ints{30, 99});
    const Series a = r->column("a");
    CHECK(a.type() == TypeId::Float64);
    CHECK(a.values<double>()[0] == 3.0);
    CHECK(a.is_null(1));

    Series zero[] = {i64s({0})};
    auto filled = indexed->reindex(keys, {}, zero[0]);
    REQUIRE(filled.has_value());
    CHECK(filled->column("a").type() == TypeId::Int64);
    CHECK(ints(filled->column("a")) == Ints{3, 0});
    CHECK(filled->column("b").values<double>()[1] == 0.0);
}

TEST_CASE("a frame with no index reindexes by position") {
    DataFrame f;
    f.names.emplace_back("a");
    f.columns.push_back(i64s({1, 2, 3}));
    Series keys[] = {i64s({2, 7})};
    auto r = f.reindex(keys);
    REQUIRE(r.has_value());
    CHECK(r->index_names().size() == 1);
    CHECK(r->index_names()[0] == "__index_level_0__");
    CHECK(r->column("a").values<double>()[0] == 3.0);
    CHECK(r->column("a").is_null(1));
}

TEST_CASE("align joins the labels as pandas") {
    const Series a = labelled(i64s({1, 2, 3}), i64s({3, 1, 2}));
    const Series b = labelled(i64s({10, 20, 30}), i64s({2, 5, 3}));
    auto outer = align_labels(a, b, JoinHow::Outer);
    REQUIRE(outer.has_value());
    CHECK(labels(outer->left) == Ints{1, 2, 3, 5});
    CHECK(labels(outer->right) == Ints{1, 2, 3, 5});
    CHECK(outer->left.type() == TypeId::Float64);
    CHECK(outer->left.values<double>()[0] == 2.0);
    CHECK(outer->left.is_null(3));
    CHECK(outer->right.is_null(0));
    CHECK(outer->right.values<double>()[3] == 20.0);

    auto inner = align_labels(a, b, JoinHow::Inner);
    REQUIRE(inner.has_value());
    CHECK(labels(inner->left) == Ints{3, 2});
    CHECK(ints(inner->left) == Ints{1, 3});
    CHECK(ints(inner->right) == Ints{30, 10});

    auto left = align_labels(a, b, JoinHow::Left);
    REQUIRE(left.has_value());
    CHECK(labels(left->right) == Ints{3, 1, 2});
    CHECK(left->right.values<double>()[0] == 30.0);
    CHECK(left->right.is_null(1));

    auto right = align_labels(a, b, JoinHow::Right);
    REQUIRE(right.has_value());
    CHECK(labels(right->left) == Ints{2, 5, 3});
    CHECK(right->left.values<double>()[0] == 3.0);
    CHECK(right->left.is_null(1));
    CHECK(ints(right->right) == Ints{10, 20, 30});
}

TEST_CASE("align meets repeated labels row by row") {
    const Series a = labelled(i64s({1, 2, 3, 4}), i64s({3, 1, 3, 2}));
    const Series b = labelled(i64s({1, 2, 3}), i64s({3, 3, 2}));
    auto outer = align_labels(a, b, JoinHow::Outer);
    REQUIRE(outer.has_value());
    CHECK(labels(outer->left) == Ints{1, 2, 3, 3, 3, 3});
    CHECK(ints(outer->left) == Ints{2, 4, 1, 1, 3, 3});
    CHECK(outer->right.is_null(0));
    CHECK(outer->right.values<double>()[1] == 3.0);
    CHECK(outer->right.values<double>()[2] == 1.0);
    CHECK(outer->right.values<double>()[3] == 2.0);
    auto inner = align_labels(a, b, JoinHow::Inner);
    REQUIRE(inner.has_value());
    CHECK(labels(inner->left) == Ints{3, 3, 3, 3, 2});
    CHECK(ints(inner->left) == Ints{1, 1, 3, 3, 4});
    CHECK(ints(inner->right) == Ints{1, 2, 1, 2, 3});
}

TEST_CASE("align leaves equal labels where they are") {
    const Series a = labelled(i64s({1, 2, 3}), i64s({3, 1, 2}));
    const Series b = labelled(i64s({4, 5, 6}), i64s({3, 1, 2}));
    for (const JoinHow how :
         {JoinHow::Inner, JoinHow::Left, JoinHow::Right, JoinHow::Outer}) {
        auto r = align_labels(a, b, how);
        REQUIRE(r.has_value());
        CHECK(labels(r->left) == Ints{3, 1, 2});
        CHECK(ints(r->right) == Ints{4, 5, 6});
    }
    CHECK_FALSE(align_labels(a, b, JoinHow::Cross).has_value());
}

TEST_CASE("align treats a positional column as 0 .. length-1") {
    const Series a = i64s({1, 2, 3});
    const Series b = i64s({4, 5});
    auto r = align_labels(a, b, JoinHow::Outer);
    REQUIRE(r.has_value());
    CHECK(labels(r->left) == Ints{0, 1, 2});
    CHECK(r->right.values<double>()[1] == 5.0);
    CHECK(r->right.is_null(2));
}

TEST_CASE("align_rows aligns the rows of two frames and keeps the rest") {
    auto make = [](dstd::StringView col, Ints values, Ints keys) {
        DataFrame f;
        f.names.emplace_back(col);
        f.columns.push_back(i64s(std::move(values)));
        f.names.emplace_back("k");
        f.columns.push_back(i64s(std::move(keys)));
        const dstd::StringView idx[] = {"k"};
        auto r = f.with_index(idx);
        REQUIRE(r.has_value());
        return std::move(*r);
    };
    const DataFrame a = make("x", {1, 2, 3}, {3, 1, 2});
    const DataFrame b = make("y", {10, 20}, {1, 9});
    auto r = align_rows(a, b, JoinHow::Outer);
    REQUIRE(r.has_value());
    CHECK(ints(r->left.column("k")) == Ints{1, 2, 3, 9});
    CHECK(ints(r->right.column("k")) == Ints{1, 2, 3, 9});
    CHECK(r->left.column("x").values<double>()[0] == 2.0);
    CHECK(r->left.column("x").is_null(3));
    CHECK(r->right.column("y").values<double>()[0] == 10.0);
    CHECK(r->right.column("y").values<double>()[3] == 20.0);
    CHECK(r->left.index_names().size() == 1);

    Series zero[] = {i64s({0})};
    auto filled = align_rows(a, b, JoinHow::Outer, zero[0]);
    REQUIRE(filled.has_value());
    CHECK(filled->left.column("x").type() == TypeId::Int64);
    CHECK(ints(filled->left.column("x")) == Ints{2, 3, 1, 0});
}

namespace {

Series stamps(dstd::Vector<std::int64_t> ticks, TimeUnit unit) {
    Series s = Series::flat(TypeId::Timestamp, ticks.data(),
                            static_cast<std::int64_t>(ticks.size()));
    s.handle()->set_time_unit(unit);
    return s;
}

Series stamp_labelled(dstd::Vector<std::int64_t> values, Series level) {
    return labelled(i64s(std::move(values)), std::move(level));
}

}  // namespace

TEST_CASE("nanosecond timestamps compare in exact integers") {
    constexpr std::int64_t T0 = 1577836800000000000;  // 2020-01-01 in ns
    const Series s = stamp_labelled(
        {1, 2, 3}, stamps({T0, T0 + 1000, T0 + 2000}, TimeUnit::Nano));
    ReindexOptions o;
    o.method = FillMethod::Nearest;
    o.tolerance_is_nanos = true;
    o.tolerance_nanos = 10;
    const Series r = reindexed(
        s, stamps({T0 + 130, T0 + 1005, T0 + 1501}, TimeUnit::Nano), o);
    CHECK(r.is_null(0));
    CHECK(r.values<double>()[1] == 2.0);
    CHECK(r.is_null(2));

    ReindexOptions near;
    near.method = FillMethod::Nearest;
    const Series m = reindexed(
        s, stamps({T0 + 499, T0 + 500, T0 + 501}, TimeUnit::Nano), near);
    CHECK(ints(m) == Ints{1, 2, 2});
}

TEST_CASE("integers above 2**53 keep their distances") {
    const std::int64_t big = std::int64_t{1} << 60;
    const Series s = labelled(i64s({1, 2}), i64s({big, big + 3}));
    ReindexOptions o;
    o.method = FillMethod::Nearest;
    o.tolerance = 1;
    const Series r = reindexed(s, i64s({big + 1, big + 2, big + 5}), o);
    CHECK(r.values<double>()[0] == 1.0);
    CHECK(r.values<double>()[1] == 2.0);
    CHECK(r.is_null(2));
    ReindexOptions plain;
    plain.method = FillMethod::Nearest;
    CHECK(ints(reindexed(s, i64s({big + 1, big + 2}), plain)) == Ints{1, 2});
}

TEST_CASE("timestamps of different units meet in the finer unit") {
    constexpr std::int64_t SEC = 1000000000;
    // Labels in microseconds, keys in nanoseconds and the other way round.
    const Series us = stamp_labelled(
        {1, 2, 3}, stamps({0, 10 * 1000000, 20 * 1000000}, TimeUnit::Micro));
    const Series ns = stamp_labelled(
        {1, 2, 3}, stamps({0, 10 * SEC, 20 * SEC}, TimeUnit::Nano));
    const Series key_ns = stamps({10 * SEC, 10 * SEC + 1, 0}, TimeUnit::Nano);
    const Series key_us = stamps({10 * 1000000, 0}, TimeUnit::Micro);
    const Series key_s = stamps({10, 0}, TimeUnit::Second);

    const Series a = reindexed(us.share(), key_ns.share());
    CHECK(a.values<double>()[0] == 2.0);
    CHECK(a.is_null(1));  // not a whole microsecond: no match, no rounding
    CHECK(a.values<double>()[2] == 1.0);
    CHECK(ints(reindexed(ns.share(), key_us.share())) == Ints{2, 1});
    CHECK(ints(reindexed(ns.share(), key_s.share())) == Ints{2, 1});
    CHECK(ints(reindexed(us.share(), key_s.share())) == Ints{2, 1});

    ReindexOptions pad;
    pad.method = FillMethod::Pad;
    const Series p = reindexed(us.share(), key_ns.share(), pad);
    CHECK(ints(p) == Ints{2, 2, 1});

    Series keys[] = {key_ns.share()};
    auto found = us.locate(keys);
    REQUIRE(found.has_value());
    CHECK(found->positions == Ints{1, 0});
    CHECK(found->missing == Ints{1});
}

TEST_CASE("a text key against number labels with a method is an error") {
    ReindexOptions o;
    o.method = FillMethod::Pad;
    const dstd::StringView words[] = {"a"};
    CHECK_FALSE(error_of(ladder(), Series::strings(words), o).empty());
}

TEST_CASE("a column with no labels reindexes by position without a hash") {
    const Series s = i64s({5, 6, 7});
    const Series r = reindexed(s, i64s({2, 0, 5, -1, 2}));
    CHECK(r.is_null(2));
    CHECK(r.is_null(3));
    CHECK(r.values<double>()[0] == 7.0);
    CHECK(r.values<double>()[4] == 7.0);
    const Series f = reindexed(s, f64s({1.0, 1.5}));
    CHECK(f.values<double>()[0] == 6.0);
    CHECK(f.is_null(1));
}
