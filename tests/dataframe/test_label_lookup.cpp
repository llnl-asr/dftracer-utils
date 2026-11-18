// SPDX-License-Identifier: MIT
// Label lookup: Series::locate, DataFrame::locate and their range forms over
// the hash a column or frame builds once from its labels.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/label_index.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

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

Series strs(dstd::Vector<dstd::StringView> v) { return Series::strings(v); }

Series labelled(Series values, dstd::Vector<Series> levels) {
    dstd::Vector<dstd::StringView> names;
    dstd::Vector<dstd::String> keep;
    for (std::size_t i = 0; i < levels.size(); ++i)
        keep.push_back(dstd::String("l") +
                       dstd::String(1, static_cast<char>('0' + i)));
    for (const dstd::String& k : keep) names.push_back(k);
    return values.with_index(names, levels);
}

Series over(Series level) {
    dstd::Vector<Series> levels;
    const std::int64_t rows = level.length();
    levels.push_back(std::move(level));
    return labelled(f64s({0, 1, 2, 3, 4, 5, 6}).slice(0, rows),
                    std::move(levels));
}

Located find(const Series& s, Series key) {
    Series keys[] = {std::move(key)};
    auto r = s.locate(keys);
    REQUIRE(r.has_value());
    return std::move(*r);
}

}  // namespace

TEST_CASE("an integer label finds its rows, duplicates in row order") {
    const Series s = over(i64s({30, 10, 20, 10, 40}));
    const Located one = find(s, i64s({10}));
    CHECK(one.positions == Ints{1, 3});
    CHECK(one.missing.empty());
    const Located list = find(s, i64s({40, 10, 40, 99, 30}));
    CHECK(list.positions == Ints{4, 1, 3, 4, 0});
    CHECK(list.missing == Ints{3});
}

TEST_CASE("labels compare by value across numeric types") {
    const Series ints = over(i64s({1, 2, 3}));
    CHECK(find(ints, f64s({2.0})).positions == Ints{1});
    CHECK(find(ints, f64s({2.5})).missing == Ints{0});
    const Series floats = over(f64s({1.0, 2.5, 3.0}));
    CHECK(find(floats, i64s({3})).positions == Ints{2});
    CHECK(find(floats, f64s({2.5})).positions == Ints{1});
    const Series wide = over(i64s({1, 2, 3}));
    const std::int32_t narrow[] = {3};
    CHECK(find(wide, Series::flat(TypeId::Int32, narrow, 1)).positions ==
          Ints{2});
    const std::uint64_t big[] = {std::numeric_limits<std::uint64_t>::max()};
    const Series huge = over(Series::flat(TypeId::Uint64, big, 1));
    CHECK(find(huge, Series::flat(TypeId::Uint64, big, 1)).positions ==
          Ints{0});
    CHECK(find(huge, i64s({-1})).missing == Ints{0});
}

TEST_CASE("a null or NaN key finds the null labels") {
    const double nan = std::nan("");
    const std::uint8_t valid[] = {0b101};
    const double v[] = {1.0, 0.0, 3.0};
    const Series labels = Series::flat(TypeId::Float64, v, 3, valid);
    const Series s = over(labels.share());
    CHECK(find(s, f64s({nan})).positions == Ints{1});
    CHECK(find(s, Series::nulls(TypeId::Int64, 1)).positions == Ints{1});
    const Series with_nan = over(f64s({1.0, nan, 3.0}));
    CHECK(find(with_nan, f64s({nan})).positions == Ints{1});
}

TEST_CASE("text labels match only text") {
    const Series s = over(strs({"b", "a", "b"}));
    CHECK(find(s, strs({"b"})).positions == Ints{0, 2});
    CHECK(find(s, strs({"z"})).missing == Ints{0});
    CHECK(find(s, i64s({1})).missing == Ints{0});
    const Series nums = over(i64s({1, 2, 3}));
    CHECK(find(nums, strs({"1"})).missing == Ints{0});
}

TEST_CASE("a MultiIndex takes a full key or a leading part of one") {
    dstd::Vector<Series> levels;
    levels.push_back(strs({"a", "a", "b", "b", "a"}));
    levels.push_back(i64s({1, 2, 1, 2, 1}));
    const Series s = labelled(f64s({0, 1, 2, 3, 4}), std::move(levels));
    Series full[] = {strs({"a", "b", "z"}), i64s({1, 2, 1})};
    auto r = s.locate(full);
    REQUIRE(r.has_value());
    CHECK(r->positions == Ints{0, 4, 3});
    CHECK(r->missing == Ints{2});
    Series partial[] = {strs({"b", "a"})};
    auto p = s.locate(partial);
    REQUIRE(p.has_value());
    CHECK(p->positions == Ints{2, 3, 0, 1, 4});
    Series too_many[] = {strs({"a"}), i64s({1}), i64s({1})};
    CHECK_FALSE(s.locate(too_many).has_value());
    Series uneven[] = {strs({"a", "b"}), i64s({1})};
    CHECK_FALSE(s.locate(uneven).has_value());
    CHECK_FALSE(s.locate({}).has_value());
}

TEST_CASE("a column with no labels is looked up by position") {
    const Series s = f64s({5, 6, 7});
    CHECK(find(s, i64s({2, 0})).positions == Ints{2, 0});
    CHECK(find(s, i64s({3, -1})).missing == Ints{0, 1});
    CHECK(find(s, f64s({1.0})).positions == Ints{1});
    CHECK(find(s, f64s({1.5})).missing == Ints{0});
    CHECK(find(s, strs({"a"})).missing == Ints{0});
}

namespace {

LabelSpan span(const Series& s, Series lo, Series hi) {
    dstd::Vector<Series> a;
    dstd::Vector<Series> b;
    if (lo.valid()) a.push_back(std::move(lo));
    if (hi.valid()) b.push_back(std::move(hi));
    auto r = s.locate_range(a, b);
    REQUIRE(r.has_value());
    return *r;
}

}  // namespace

TEST_CASE("a range on a sorted index is a binary search, both ends included") {
    const Series s = over(i64s({10, 20, 20, 30, 40}));
    LabelSpan r = span(s, i64s({20}), i64s({30}));
    CHECK(r.offset == 1);
    CHECK(r.length == 3);
    r = span(s, i64s({15}), i64s({35}));
    CHECK(r.offset == 1);
    CHECK(r.length == 3);
    r = span(s, Series{}, i64s({20}));
    CHECK(r.offset == 0);
    CHECK(r.length == 3);
    r = span(s, i64s({30}), Series{});
    CHECK(r.offset == 3);
    CHECK(r.length == 2);
    r = span(s, i64s({40}), i64s({10}));
    CHECK(r.length == 0);
    r = span(s, f64s({20.5}), f64s({39.5}));
    CHECK(r.offset == 3);
    CHECK(r.length == 1);
}

TEST_CASE("a range on a decreasing index runs from the larger label") {
    const Series s = over(i64s({50, 40, 30, 20, 10}));
    const LabelSpan r = span(s, i64s({40}), i64s({20}));
    CHECK(r.offset == 1);
    CHECK(r.length == 3);
    const LabelSpan q = span(s, i64s({45}), i64s({15}));
    CHECK(q.offset == 1);
    CHECK(q.length == 3);
}

TEST_CASE("a range on an unsorted index needs bounds that are unique labels") {
    const Series s = over(i64s({30, 10, 20, 40, 10}));
    Series repeated[] = {i64s({10})};
    CHECK_FALSE(s.locate_range(repeated, {}).has_value());
    Series gone[] = {i64s({99})};
    CHECK_FALSE(s.locate_range(gone, {}).has_value());
    Series lo[] = {i64s({30})};
    Series hi[] = {i64s({20})};
    auto r = s.locate_range(lo, hi);
    REQUIRE(r.has_value());
    CHECK(r->offset == 0);
    CHECK(r->length == 3);
    Series back_lo[] = {i64s({20})};
    Series back_hi[] = {i64s({30})};
    auto e = s.locate_range(back_lo, back_hi);
    REQUIRE(e.has_value());
    CHECK(e->length == 0);
}

TEST_CASE("a range between types with no order is an error") {
    const Series s = over(i64s({1, 2, 3}));
    Series text[] = {strs({"a"})};
    CHECK_FALSE(s.locate_range(text, {}).has_value());
}

TEST_CASE("a range on a MultiIndex needs it sorted") {
    dstd::Vector<Series> sorted;
    sorted.push_back(strs({"a", "a", "b", "b"}));
    sorted.push_back(i64s({1, 2, 1, 2}));
    const Series s = labelled(f64s({0, 1, 2, 3}), std::move(sorted));
    Series lo[] = {strs({"a"}), i64s({2})};
    Series hi[] = {strs({"b"})};
    auto r = s.locate_range(lo, hi);
    REQUIRE(r.has_value());
    CHECK(r->offset == 1);
    CHECK(r->length == 3);
    dstd::Vector<Series> mixed;
    mixed.push_back(strs({"b", "a", "b", "a"}));
    mixed.push_back(i64s({1, 1, 2, 2}));
    const Series u = labelled(f64s({0, 1, 2, 3}), std::move(mixed));
    CHECK_FALSE(u.locate_range(lo, hi).has_value());
}

TEST_CASE("a positional column has the range 0 to rows-1") {
    const Series s = f64s({5, 6, 7, 8});
    LabelSpan r = span(s, i64s({1}), i64s({2}));
    CHECK(r.offset == 1);
    CHECK(r.length == 2);
    r = span(s, f64s({0.5}), f64s({10.0}));
    CHECK(r.offset == 1);
    CHECK(r.length == 3);
    r = span(s, i64s({-4}), i64s({-1}));
    CHECK(r.length == 0);
}

TEST_CASE("a frame is looked up by its index columns") {
    DataFrame df;
    df.names.emplace_back("k");
    df.columns.push_back(i64s({3, 1, 3, 2}));
    df.names.emplace_back("v");
    df.columns.push_back(f64s({0, 1, 2, 3}));
    Series key[] = {i64s({3, 9})};
    auto unindexed = df.locate(key);
    REQUIRE(unindexed.has_value());
    CHECK(unindexed->positions == Ints{3});
    CHECK(unindexed->missing == Ints{1});
    const dstd::StringView index[] = {"k"};
    auto indexed = df.with_index(index);
    REQUIRE(indexed.has_value());
    auto r = indexed->locate(key);
    REQUIRE(r.has_value());
    CHECK(r->positions == Ints{0, 2});
    CHECK(r->missing == Ints{1});
}

TEST_CASE("the hash is built once for a column, its copies and every thread") {
    const Series s = over(i64s({5, 4, 3, 2, 1}));
    const std::uint64_t before = label_index_builds();
    Series key[] = {i64s({3})};
    CHECK(s.locate(key)->positions == Ints{2});
    const Series copy = s.share();
    CHECK(copy.locate(key)->positions == Ints{2});
    Series other[] = {i64s({1})};
    CHECK(s.locate(other)->positions == Ints{4});
    CHECK(label_index_builds() == before + 1);

    const Series fresh = over(i64s({9, 8, 7, 6, 5}));
    const std::uint64_t base = label_index_builds();
    std::atomic<int> good{0};
    dstd::Vector<dstd::Thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            for (int round = 0; round < 200; ++round) {
                Series k[] = {i64s({9 - (round + t) % 5})};
                auto r = fresh.locate(k);
                if (r && r->positions.size() == 1 &&
                    r->positions[0] == (round + t) % 5)
                    ++good;
            }
        });
    for (dstd::Thread& t : threads) t.join();
    CHECK(good.load() == 800);
    CHECK(label_index_builds() == base + 1);
}

TEST_CASE("a frame and its share copy build the hash once") {
    DataFrame df;
    df.names.emplace_back("k");
    df.columns.push_back(i64s({3, 1, 3, 2}));
    const dstd::StringView index[] = {"k"};
    auto indexed = df.with_index(index);
    REQUIRE(indexed.has_value());
    const DataFrame copy = indexed->share();
    const std::uint64_t before = label_index_builds();
    Series key[] = {i64s({3})};
    CHECK(indexed->locate(key)->positions == Ints{0, 2});
    CHECK(copy.locate(key)->positions == Ints{0, 2});
    CHECK(label_index_builds() == before + 1);
}

TEST_CASE("a Bool label is not a number") {
    const std::uint8_t bits[] = {0b01};
    const Series flags = Series::flat(TypeId::Bool, bits, 2);
    const Series s = over(flags.share());
    CHECK(find(s, Series::flat(TypeId::Bool, bits, 2)).positions == Ints{0, 1});
    CHECK(find(s, i64s({1})).missing == Ints{0});
    CHECK(find(s, f64s({1.0})).missing == Ints{0});
    const Series nums = over(i64s({1, 0}));
    CHECK(find(nums, Series::flat(TypeId::Bool, bits, 1)).missing == Ints{0});
}
