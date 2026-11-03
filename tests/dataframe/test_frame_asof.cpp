// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/algorithm.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <utility>

using namespace dftracer::utils;

namespace df = dftracer::utils::dataframe;
using df::AsofDirection;
using df::DataFrame;
using df::Series;
using df::TypeId;

namespace {

using I = dstd::Optional<std::int64_t>;
using S = dstd::Optional<dstd::String>;
const I NI = dstd::nullopt;
const S NS = dstd::nullopt;

template <class T>
dstd::Vector<std::uint8_t> validity(const dstd::Vector<dstd::Optional<T>>& v) {
    dstd::Vector<std::uint8_t> bits((v.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i]) bits[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
    return bits;
}

template <class T>
dstd::Vector<T> values_of(const dstd::Vector<I>& v) {
    dstd::Vector<T> out(v.size(), T{});
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i]) out[i] = static_cast<T>(*v[i]);
    return out;
}

// Float64 stores v * 0.5 so a double column holds fractional times.
Series typed(TypeId type, const dstd::Vector<I>& v) {
    const auto n = static_cast<std::int64_t>(v.size());
    const dstd::Vector<std::uint8_t> bits = validity(v);
    switch (type) {
        case TypeId::Int32: {
            const auto d = values_of<std::int32_t>(v);
            return Series::flat(type, d.data(), n, bits.data());
        }
        case TypeId::Uint64: {
            const auto d = values_of<std::uint64_t>(v);
            return Series::flat(type, d.data(), n, bits.data());
        }
        case TypeId::Float64: {
            auto d = values_of<double>(v);
            for (double& x : d) x *= 0.5;
            return Series::flat(type, d.data(), n, bits.data());
        }
        default: {
            const auto d = values_of<std::int64_t>(v);
            return Series::flat(TypeId::Int64, d.data(), n, bits.data());
        }
    }
}

double tval(TypeId type, std::int64_t v) {
    return type == TypeId::Float64 ? static_cast<double>(v) * 0.5
                                   : static_cast<double>(v);
}

Series i64s(const dstd::Vector<I>& v) { return typed(TypeId::Int64, v); }

Series u64s(const dstd::Vector<std::uint64_t>& v) {
    return Series::flat(TypeId::Uint64, v.data(),
                        static_cast<std::int64_t>(v.size()));
}

Series f64s(const dstd::Vector<double>& v) {
    return Series::flat_f64(v.data(), static_cast<std::int64_t>(v.size()));
}

Series strs(const dstd::Vector<S>& v) {
    dstd::Vector<dstd::StringView> views;
    views.reserve(v.size());
    for (const S& s : v)
        views.emplace_back(s ? dstd::StringView(*s) : dstd::StringView());
    const dstd::Vector<std::uint8_t> bits = validity(v);
    return Series::strings(dstd::Span<const dstd::StringView>(views),
                           bits.data());
}

template <class... Cols>
DataFrame frame(dstd::Vector<dstd::String> names, Cols&&... cols) {
    DataFrame out;
    out.names = std::move(names);
    (out.columns.push_back(std::forward<Cols>(cols)), ...);
    return out;
}

dstd::Vector<I> i64_col(const DataFrame& d, const dstd::String& name) {
    Series c = d.column(name);
    REQUIRE(c.valid());
    c = c.materialize();
    REQUIRE(c.type() == TypeId::Int64);
    dstd::Vector<I> out;
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i))
            out.emplace_back(dstd::nullopt);
        else
            out.emplace_back(c.data<std::int64_t>()[i]);
    }
    return out;
}

dstd::Vector<S> str_col(const DataFrame& d, const dstd::String& name) {
    Series c = d.column(name);
    REQUIRE(c.valid());
    c = c.materialize();
    dstd::Vector<S> out;
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i))
            out.emplace_back(dstd::nullopt);
        else
            out.emplace_back(dstd::String(c.string_at(i)));
    }
    return out;
}

dstd::Vector<dstd::String> str_list(const DataFrame& d,
                                    const dstd::String& name,
                                    std::int64_t row) {
    Series c = d.column(name);
    REQUIRE(c.valid());
    c = c.materialize();
    REQUIRE(c.type() == TypeId::List);
    const std::int32_t* off = c.offsets();
    REQUIRE(off != nullptr);
    const Series vals = c.child(0).materialize();
    dstd::Vector<dstd::String> out;
    for (std::int32_t p = off[row]; p < off[row + 1]; ++p)
        out.emplace_back(vals.string_at(p));
    return out;
}

DataFrame ts_frame(const dstd::Vector<I>& ts) {
    return frame({"ts"}, i64s(ts));
}

DataFrame ts_vals(const dstd::Vector<I>& ts, const dstd::Vector<S>& vals) {
    return frame({"ts", "val"}, i64s(ts), strs(vals));
}

DataFrame asof_g(const DataFrame& l, const DataFrame& r, AsofDirection dir,
                 dstd::Optional<double> tol = dstd::nullopt) {
    return df::asof(l, r, "ts", {}, dir, tol);
}

DataFrame points(const dstd::Vector<I>& p) { return frame({"point"}, i64s(p)); }

DataFrame spans(const dstd::Vector<I>& lo, const dstd::Vector<I>& hi,
                const dstd::Vector<S>& vals) {
    return frame({"lo", "hi", "val"}, i64s(lo), i64s(hi), strs(vals));
}

DataFrame interval_g(const DataFrame& l, const DataFrame& r, bool outer) {
    return df::interval(l, r, "point", "lo", "hi", {}, outer);
}

}  // namespace

TEST_CASE("asof - backward basic (no by keys)") {
    const DataFrame out =
        asof_g(ts_frame({3, 10, 20, 30}), ts_vals({5, 15, 25}, {"a", "b", "c"}),
               AsofDirection::Backward);
    CHECK(out.names == dstd::Vector<dstd::String>{"ts", "val"});
    CHECK(i64_col(out, "ts") == dstd::Vector<I>{3, 10, 20, 30});
    CHECK(str_col(out, "val") == dstd::Vector<S>{NS, "a", "b", "c"});
}

TEST_CASE("asof - forward") {
    const DataFrame out =
        asof_g(ts_frame({3, 10, 20, 30}), ts_vals({5, 15, 25}, {"a", "b", "c"}),
               AsofDirection::Forward);
    CHECK(str_col(out, "val") == dstd::Vector<S>{"a", "b", "c", NS});
}

TEST_CASE("asof - nearest with backward tie-break") {
    SUBCASE("closest wins") {
        const DataFrame out =
            asof_g(ts_frame({10, 20}), ts_vals({8, 14, 25}, {"a", "b", "c"}),
                   AsofDirection::Nearest);
        CHECK(str_col(out, "val") == dstd::Vector<S>{"a", "c"});
    }
    SUBCASE("tie prefers backward") {
        const DataFrame out =
            asof_g(ts_frame({11}), ts_vals({6, 16}, {"a", "b"}),
                   AsofDirection::Nearest);
        CHECK(str_col(out, "val") == dstd::Vector<S>{"a"});
    }
}

TEST_CASE("asof - equal right times follow merge_asof") {
    const DataFrame r = ts_vals({8, 8, 14, 14}, {"a", "b", "c", "d"});
    auto pick = [&](AsofDirection dir, std::int64_t t) {
        const DataFrame out = asof_g(ts_frame({t}), r, dir);
        REQUIRE(out.num_rows() == 1);
        return str_col(out, "val").front();
    };
    CHECK(pick(AsofDirection::Backward, 8) == S("b"));
    CHECK(pick(AsofDirection::Forward, 8) == S("a"));
    CHECK(pick(AsofDirection::Nearest, 8) == S("b"));
    CHECK(pick(AsofDirection::Nearest, 14) == S("d"));
    CHECK(pick(AsofDirection::Nearest, 10) == S("b"));
    CHECK(pick(AsofDirection::Nearest, 11) == S("b"));
    CHECK(pick(AsofDirection::Nearest, 12) == S("c"));
}

TEST_CASE("asof - nearest takes the last equal time at or before the left") {
    const DataFrame r = ts_vals({16, 6, 6, 16, 6}, {"c", "a", "b", "d", "e"});
    auto pick = [&](AsofDirection dir, std::int64_t t) {
        const DataFrame out = asof_g(ts_frame({t}), r, dir);
        REQUIRE(out.num_rows() == 1);
        return str_col(out, "val").front();
    };
    CHECK(pick(AsofDirection::Nearest, 6) == S("e"));
    CHECK(pick(AsofDirection::Nearest, 16) == S("d"));
    CHECK(pick(AsofDirection::Nearest, 11) == S("e"));
    CHECK(pick(AsofDirection::Nearest, 10) == S("e"));
    CHECK(pick(AsofDirection::Nearest, 12) == S("c"));
    CHECK(pick(AsofDirection::Backward, 16) == S("d"));
    CHECK(pick(AsofDirection::Forward, 16) == S("c"));
    CHECK(pick(AsofDirection::Forward, 6) == S("a"));
}

TEST_CASE("asof - by-key partitioning") {
    const DataFrame l = frame({"pid", "ts"}, i64s({1, 2}), i64s({10, 10}));
    const DataFrame r = frame({"pid", "ts", "val"}, i64s({1, 1, 2}),
                              i64s({5, 8, 3}), strs({"a1", "a2", "b1"}));
    const DataFrame out =
        df::asof(l, r, "ts", {"pid"}, AsofDirection::Backward, dstd::nullopt);
    CHECK(out.names == dstd::Vector<dstd::String>{"pid", "ts", "val"});
    CHECK(i64_col(out, "pid") == dstd::Vector<I>{1, 2});
    CHECK(str_col(out, "val") == dstd::Vector<S>{"a2", "b1"});
}

TEST_CASE("asof - a null by key matches a null by key") {
    const DataFrame l = frame({"k", "ts"}, i64s({NI, 1}), i64s({10, 10}));
    const DataFrame r = frame({"k", "ts", "val"}, i64s({1, NI}), i64s({5, 7}),
                              strs({"a", "b"}));
    const DataFrame out =
        df::asof(l, r, "ts", {"k"}, AsofDirection::Backward, dstd::nullopt);
    CHECK(i64_col(out, "k") == dstd::Vector<I>{1, NI});
    CHECK(str_col(out, "val") == dstd::Vector<S>{"a", "b"});
}

TEST_CASE("asof - tolerance") {
    const DataFrame l = ts_frame({10});
    const DataFrame r = ts_vals({4}, {"a"});
    SUBCASE("just outside tol -> null") {
        CHECK(str_col(asof_g(l, r, AsofDirection::Backward, 5), "val") ==
              dstd::Vector<S>{NS});
    }
    SUBCASE("just inside tol -> match") {
        CHECK(str_col(asof_g(l, r, AsofDirection::Backward, 6), "val") ==
              dstd::Vector<S>{"a"});
    }
}

TEST_CASE("asof - far-apart ts near int64 extremes (no signed overflow)") {
    const std::int64_t hi = std::numeric_limits<std::int64_t>::max();
    const std::int64_t lo = std::numeric_limits<std::int64_t>::min();
    SUBCASE("backward across the full int64 span, out of tol -> null") {
        const DataFrame out = asof_g(ts_frame({hi}), ts_vals({lo}, {"a"}),
                                     AsofDirection::Backward, 1000);
        CHECK(str_col(out, "val") == dstd::Vector<S>{NS});
    }
    SUBCASE("nearest picks the closer extreme without overflow") {
        const DataFrame out =
            asof_g(ts_frame({0}), ts_vals({lo, hi}, {"neg", "pos"}),
                   AsofDirection::Nearest);
        CHECK(str_col(out, "val") == dstd::Vector<S>{"pos"});
    }
    SUBCASE("match near the top of the range") {
        const DataFrame out = asof_g(ts_frame({hi}), ts_vals({hi - 5}, {"a"}),
                                     AsofDirection::Backward, 5);
        CHECK(str_col(out, "val") == dstd::Vector<S>{"a"});
    }
}

TEST_CASE("asof - uint64 times near the top of the range") {
    const std::uint64_t top = std::numeric_limits<std::uint64_t>::max();
    const DataFrame l = frame({"ts"}, u64s({top}));
    const DataFrame r =
        frame({"ts", "val"}, u64s({0, top - 3}), strs({"zero", "near"}));
    CHECK(str_col(asof_g(l, r, AsofDirection::Nearest), "val") ==
          dstd::Vector<S>{"near"});
    CHECK(str_col(asof_g(l, r, AsofDirection::Backward, 3), "val") ==
          dstd::Vector<S>{"near"});
    CHECK(str_col(asof_g(l, r, AsofDirection::Backward, 2), "val") ==
          dstd::Vector<S>{NS});
}

TEST_CASE("asof - a fractional tolerance") {
    SUBCASE("integer times take its floor") {
        const DataFrame l = ts_frame({10});
        const DataFrame r = ts_vals({4}, {"a"});
        CHECK(str_col(asof_g(l, r, AsofDirection::Backward, 5.99), "val") ==
              dstd::Vector<S>{NS});
        CHECK(str_col(asof_g(l, r, AsofDirection::Backward, 6.5), "val") ==
              dstd::Vector<S>{"a"});
    }
    SUBCASE("float times compare it exactly") {
        const DataFrame l = frame({"ts"}, f64s({1.0}));
        const DataFrame r = frame({"ts", "val"}, f64s({0.995}), strs({"a"}));
        CHECK(str_col(asof_g(l, r, AsofDirection::Backward, 0.004), "val") ==
              dstd::Vector<S>{NS});
        CHECK(str_col(asof_g(l, r, AsofDirection::Backward, 0.006), "val") ==
              dstd::Vector<S>{"a"});
    }
    SUBCASE("uint64 distances past 2^53 stay exact") {
        const std::uint64_t top = std::numeric_limits<std::uint64_t>::max();
        const DataFrame l = frame({"ts"}, u64s({top}));
        const DataFrame r = frame({"ts", "val"}, u64s({top - 3}), strs({"a"}));
        CHECK(str_col(asof_g(l, r, AsofDirection::Backward, 2.9), "val") ==
              dstd::Vector<S>{NS});
        CHECK(str_col(asof_g(l, r, AsofDirection::Backward, 1e30), "val") ==
              dstd::Vector<S>{"a"});
    }
}

TEST_CASE("asof - float ts rejects a negative tolerance") {
    const DataFrame l = frame({"ts"}, f64s({10.0}));
    const DataFrame r = frame({"ts", "val"}, f64s({10.0}), strs({"a"}));
    CHECK(str_col(asof_g(l, r, AsofDirection::Backward, -1), "val") ==
          dstd::Vector<S>{NS});
    CHECK(str_col(asof_g(l, r, AsofDirection::Backward, 0), "val") ==
          dstd::Vector<S>{"a"});
}

TEST_CASE("asof - null ts semantics") {
    SUBCASE("left null ts -> null right") {
        const DataFrame out =
            asof_g(ts_vals({NI, 20}, {"L0", "L1"}),
                   ts_vals({5, 15}, {"a", "b"}), AsofDirection::Backward);
        CHECK(out.names ==
              dstd::Vector<dstd::String>{"ts", "val", "val_right"});
        CHECK(i64_col(out, "ts") == dstd::Vector<I>{20, NI});
        CHECK(str_col(out, "val") == dstd::Vector<S>{"L1", "L0"});
        CHECK(str_col(out, "val_right") == dstd::Vector<S>{"b", NS});
    }
    SUBCASE("right null ts is not a candidate") {
        const DataFrame out =
            asof_g(ts_frame({10}), ts_vals({NI, 5}, {"skip", "a"}),
                   AsofDirection::Backward);
        CHECK(str_col(out, "val") == dstd::Vector<S>{"a"});
    }
}

TEST_CASE("asof - determinism across input order") {
    const DataFrame o1 =
        asof_g(ts_frame({10, 20, 30}), ts_vals({5, 15, 25}, {"a", "b", "c"}),
               AsofDirection::Backward);
    const DataFrame o2 =
        asof_g(ts_frame({30, 10, 20}), ts_vals({25, 5, 15}, {"c", "a", "b"}),
               AsofDirection::Backward);
    CHECK(i64_col(o1, "ts") == dstd::Vector<I>{10, 20, 30});
    CHECK(i64_col(o1, "ts") == i64_col(o2, "ts"));
    CHECK(str_col(o1, "val") == str_col(o2, "val"));
}

TEST_CASE("asof - list<utf8> value passthrough") {
    const DataFrame r = frame({"ts", "tags"}, i64s({5, 15}),
                              Series::list({0, 2, 3}, strs({"p", "q", "r"})));
    const DataFrame out =
        asof_g(ts_frame({10, 20}), r, AsofDirection::Backward);
    CHECK(out.names == dstd::Vector<dstd::String>{"ts", "tags"});
    REQUIRE(out.num_rows() == 2);
    CHECK(str_list(out, "tags", 0) == dstd::Vector<dstd::String>{"p", "q"});
    CHECK(str_list(out, "tags", 1) == dstd::Vector<dstd::String>{"r"});
}

TEST_CASE("asof - an exact ts match is taken") {
    const DataFrame out =
        asof_g(ts_frame({10}), ts_vals({5, 10}, {"older", "exact"}),
               AsofDirection::Backward);
    CHECK(str_col(out, "val") == dstd::Vector<S>{"exact"});
}

TEST_CASE("asof - errors") {
    const DataFrame l = ts_frame({1});
    const DataFrame r = ts_vals({1}, {"a"});
    CHECK_THROWS_AS(
        df::asof(l, r, "nope", {}, AsofDirection::Backward, dstd::nullopt),
        std::out_of_range);
    CHECK_THROWS_AS(
        df::asof(l, r, "ts", {"nope"}, AsofDirection::Backward, dstd::nullopt),
        std::out_of_range);
    CHECK_THROWS_AS(
        asof_g(frame({"ts"}, f64s({1.0})), r, AsofDirection::Backward),
        std::invalid_argument);
    CHECK_THROWS_AS(asof_g(frame({"ts"}, strs({"x"})),
                           frame({"ts", "val"}, strs({"x"}), strs({"a"})),
                           AsofDirection::Backward),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        df::asof(frame({"k", "ts"}, i64s({1}), i64s({1})),
                 frame({"k", "ts", "val"}, strs({"1"}), i64s({1}), strs({"a"})),
                 "ts", {"k"}, AsofDirection::Backward, dstd::nullopt),
        std::invalid_argument);
}

TEST_CASE("interval - point in a single interval") {
    const DataFrame l = points({5, 15, 25});
    const DataFrame r = spans({0, 20}, {10, 30}, {"i0", "i1"});
    SUBCASE("schema: left cols then right non-lo/hi value cols") {
        CHECK(interval_g(l, r, false).names ==
              dstd::Vector<dstd::String>{"point", "val"});
    }
    SUBCASE("inner drops the uncovered point") {
        const DataFrame out = interval_g(l, r, false);
        CHECK(i64_col(out, "point") == dstd::Vector<I>{5, 25});
        CHECK(str_col(out, "val") == dstd::Vector<S>{"i0", "i1"});
    }
    SUBCASE("outer emits the uncovered point with null right") {
        const DataFrame out = interval_g(l, r, true);
        CHECK(i64_col(out, "point") == dstd::Vector<I>{5, 15, 25});
        CHECK(str_col(out, "val") == dstd::Vector<S>{"i0", NS, "i1"});
    }
}

TEST_CASE("interval - nested/overlapping emit one row per interval") {
    const DataFrame out = interval_g(
        points({5}), spans({0, 2}, {10, 8}, {"outer", "inner"}), false);
    CHECK(i64_col(out, "point") == dstd::Vector<I>{5, 5});
    CHECK(str_col(out, "val") == dstd::Vector<S>{"outer", "inner"});
}

TEST_CASE("interval - closed boundaries match at lo and hi") {
    const DataFrame out =
        interval_g(points({0, 10}), spans({0}, {10}, {"span"}), false);
    CHECK(i64_col(out, "point") == dstd::Vector<I>{0, 10});
    CHECK(str_col(out, "val") == dstd::Vector<S>{"span", "span"});
}

TEST_CASE("interval - by-key partitioning") {
    const DataFrame l = frame({"pid", "point"}, i64s({1, 2}), i64s({5, 5}));
    const DataFrame r = frame({"pid", "lo", "hi", "val"}, i64s({1, 2}),
                              i64s({0, 0}), i64s({10, 10}), strs({"p1", "p2"}));
    const DataFrame out =
        df::interval(l, r, "point", "lo", "hi", {"pid"}, false);
    CHECK(out.names == dstd::Vector<dstd::String>{"pid", "point", "val"});
    CHECK(i64_col(out, "pid") == dstd::Vector<I>{1, 2});
    CHECK(str_col(out, "val") == dstd::Vector<S>{"p1", "p2"});
}

TEST_CASE("interval - a null by key matches a null by key") {
    const DataFrame l = frame({"k", "point"}, i64s({NI, 1}), i64s({5, 5}));
    const DataFrame r = frame({"k", "lo", "hi", "val"}, i64s({NI}), i64s({0}),
                              i64s({10}), strs({"n"}));
    const DataFrame out = df::interval(l, r, "point", "lo", "hi", {"k"}, true);
    CHECK(i64_col(out, "k") == dstd::Vector<I>{1, NI});
    CHECK(str_col(out, "val") == dstd::Vector<S>{NS, "n"});
}

TEST_CASE("interval - determinism across input order") {
    const DataFrame o1 = interval_g(
        points({5, 25}), spans({0, 20}, {10, 30}, {"i0", "i1"}), true);
    const DataFrame o2 = interval_g(
        points({25, 5}), spans({20, 0}, {30, 10}, {"i1", "i0"}), true);
    CHECK(i64_col(o1, "point") == dstd::Vector<I>{5, 25});
    CHECK(i64_col(o1, "point") == i64_col(o2, "point"));
    CHECK(str_col(o1, "val") == str_col(o2, "val"));
}

TEST_CASE("interval - list<utf8> value passthrough") {
    const DataFrame r = frame({"lo", "hi", "tags"}, i64s({0}), i64s({10}),
                              Series::list({0, 2}, strs({"p", "q"})));
    const DataFrame out = interval_g(points({5}), r, false);
    REQUIRE(out.num_rows() == 1);
    CHECK(str_list(out, "tags", 0) == dstd::Vector<dstd::String>{"p", "q"});
}

TEST_CASE("interval - an interval covering no point is dropped") {
    const DataFrame out = interval_g(
        points({5}), spans({0, 20}, {10, 30}, {"hit", "miss"}), false);
    CHECK(i64_col(out, "point") == dstd::Vector<I>{5});
    CHECK(str_col(out, "val") == dstd::Vector<S>{"hit"});
}

TEST_CASE("interval - null semantics") {
    SUBCASE("null point matches nothing") {
        const DataFrame l =
            frame({"ts", "val"}, i64s({NI, 5}), strs({"L0", "L1"}));
        const DataFrame out = df::interval(l, spans({0}, {10}, {"span"}), "ts",
                                           "lo", "hi", {}, true);
        CHECK(out.names ==
              dstd::Vector<dstd::String>{"ts", "val", "val_right"});
        CHECK(i64_col(out, "ts") == dstd::Vector<I>{5, NI});
        CHECK(str_col(out, "val_right") == dstd::Vector<S>{"span", NS});
    }
    SUBCASE("null lo interval is never active") {
        const DataFrame out =
            interval_g(points({5}), spans({NI}, {10}, {"skip"}), false);
        CHECK(out.num_rows() == 0);
    }
    SUBCASE("null hi interval is never active") {
        const DataFrame out =
            interval_g(points({5}), spans({0}, {NI}, {"skip"}), false);
        CHECK(out.num_rows() == 0);
    }
}

TEST_CASE("interval - errors") {
    const DataFrame l = points({5});
    const DataFrame r = spans({0}, {10}, {"a"});
    CHECK_THROWS_AS(df::interval(l, r, "nope", "lo", "hi", {}, false),
                    std::out_of_range);
    CHECK_THROWS_AS(df::interval(l, r, "point", "lo", "nope", {}, false),
                    std::out_of_range);
    CHECK_THROWS_AS(df::interval(frame({"point"}, f64s({5.0})), r, "point",
                                 "lo", "hi", {}, false),
                    std::invalid_argument);
    CHECK_THROWS_AS(df::interval(frame({"point"}, strs({"5"})),
                                 frame({"lo", "hi"}, strs({"0"}), strs({"9"})),
                                 "point", "lo", "hi", {}, false),
                    std::invalid_argument);
    CHECK_THROWS_AS(df::interval(frame({"k", "point"}, i64s({1}), i64s({5})),
                                 frame({"k", "lo", "hi"}, strs({"1"}),
                                       i64s({0}), i64s({10})),
                                 "point", "lo", "hi", {"k"}, false),
                    std::invalid_argument);
}

namespace {

struct Rows {
    dstd::Vector<I> ki;
    dstd::Vector<I> ks;
    dstd::Vector<I> t;
    dstd::Vector<I> hi;
    std::size_t size() const { return t.size(); }
};

const dstd::Vector<TypeId> TIME_TYPES{TypeId::Int64, TypeId::Int32,
                                      TypeId::Uint64, TypeId::Float64};

int cmp_null_last(const I& a, const I& b) {
    if (!a || !b) return a ? -1 : (b ? 1 : 0);
    return *a < *b ? -1 : (*a > *b ? 1 : 0);
}

Rows random_rows(std::mt19937& rng, std::int64_t base) {
    std::uniform_int_distribution<int> percent(0, 99);
    auto pick = [&](std::int64_t lo, std::int64_t hi) -> I {
        if (percent(rng) < 15) return dstd::nullopt;
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    };
    const int n = std::uniform_int_distribution<int>(1, 10)(rng);
    Rows r;
    for (int i = 0; i < n; ++i) {
        r.ki.push_back(pick(0, 2));
        r.ks.push_back(pick(0, 2));
        r.t.push_back(pick(base, base + 12));
        I h = pick(base, base + 12);
        if (h && r.t.back())
            h = std::max(
                base, *r.t.back() + std::uniform_int_distribution<std::int64_t>(
                                        -1, 4)(rng));
        r.hi.push_back(h);
    }
    return r;
}

dstd::Vector<dstd::String> by_names(int by_mode) {
    dstd::Vector<dstd::String> out;
    if (by_mode & 1) out.emplace_back("ki");
    if (by_mode & 2) out.emplace_back("ks");
    return out;
}

DataFrame keyed(const Rows& rows, const dstd::String& id, int by_mode) {
    dstd::Vector<I> ids;
    for (std::size_t i = 0; i < rows.size(); ++i)
        ids.emplace_back(static_cast<std::int64_t>(i));
    DataFrame out;
    out.names.push_back(id);
    out.columns.push_back(i64s(ids));
    if (by_mode & 1) {
        out.names.emplace_back("ki");
        out.columns.push_back(i64s(rows.ki));
    }
    if (by_mode & 2) {
        dstd::Vector<S> ks;
        for (const I& k : rows.ks)
            ks.push_back(k ? S("k" + dstd::to_string(*k)) : NS);
        out.names.emplace_back("ks");
        out.columns.push_back(strs(ks));
    }
    return out;
}

bool same_keys(const Rows& l, std::size_t i, const Rows& r, std::size_t j,
               int by_mode) {
    return ((by_mode & 1) == 0 || l.ki[i] == r.ki[j]) &&
           ((by_mode & 2) == 0 || l.ks[i] == r.ks[j]);
}

dstd::Vector<std::size_t> left_order(const Rows& l, int by_mode) {
    dstd::Vector<std::size_t> order(l.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    dstd::stable_sort(order.begin(), order.end(),
                      [&](std::size_t a, std::size_t b) {
                          int c = 0;
                          if (by_mode & 1) c = cmp_null_last(l.ki[a], l.ki[b]);
                          if (c == 0 && (by_mode & 2))
                              c = cmp_null_last(l.ks[a], l.ks[b]);
                          if (c == 0) c = cmp_null_last(l.t[a], l.t[b]);
                          return c < 0;
                      });
    return order;
}

// Backward: the largest time <= x, the last row among equals. Forward: the
// smallest time >= x, the first row among equals. Nearest: the closer of the
// two, Backward on an equal distance. Tolerance applies to the pick.
I asof_ref(const Rows& l, std::size_t i, const Rows& r, TypeId type,
           int by_mode, AsofDirection dir, dstd::Optional<std::int64_t> tol) {
    if (!l.t[i]) return NI;
    const double x = tval(type, *l.t[i]);
    dstd::Optional<std::size_t> back;
    dstd::Optional<std::size_t> fwd;
    for (std::size_t j = 0; j < r.size(); ++j) {
        if (!r.t[j] || !same_keys(l, i, r, j, by_mode)) continue;
        const double y = tval(type, *r.t[j]);
        if (y <= x && (!back || y >= tval(type, *r.t[*back]))) back = j;
        if (y >= x && (!fwd || y < tval(type, *r.t[*fwd]))) fwd = j;
    }
    const auto dist = [&](std::size_t j) {
        return std::fabs(x - tval(type, *r.t[j]));
    };
    dstd::Optional<std::size_t> pick;
    if (dir == AsofDirection::Backward)
        pick = back;
    else if (dir == AsofDirection::Forward)
        pick = fwd;
    else if (!back)
        pick = fwd;
    else if (!fwd)
        pick = back;
    else
        pick = dist(*back) <= dist(*fwd) ? back : fwd;
    if (!pick) return NI;
    if (tol && (*tol < 0 || dist(*pick) > static_cast<double>(*tol))) return NI;
    return static_cast<std::int64_t>(*pick);
}

}  // namespace

TEST_CASE("asof matches a nested-loop reference") {
    std::mt19937 rng(20260929);
    const AsofDirection DIRS[] = {AsofDirection::Backward,
                                  AsofDirection::Forward,
                                  AsofDirection::Nearest};
    const dstd::Optional<std::int64_t> TOLS[] = {dstd::nullopt, 0, 2};
    for (TypeId type : TIME_TYPES)
        for (int by_mode = 0; by_mode < 4; ++by_mode)
            for (AsofDirection dir : DIRS)
                for (const auto& tol : TOLS)
                    for (int trial = 0; trial < 25; ++trial) {
                        const std::int64_t base =
                            type == TypeId::Uint64 ? 0 : -6;
                        const Rows l = random_rows(rng, base);
                        const Rows r = random_rows(rng, base);
                        DataFrame left = keyed(l, "lid", by_mode);
                        left.names.emplace_back("t");
                        left.columns.push_back(typed(type, l.t));
                        DataFrame right = keyed(r, "rid", by_mode);
                        right.names.emplace_back("t");
                        right.columns.push_back(typed(type, r.t));
                        INFO("type=" << static_cast<int>(type)
                                     << " by_mode=" << by_mode
                                     << " dir=" << static_cast<int>(dir)
                                     << " tol=" << (tol ? *tol : -99)
                                     << " trial=" << trial);

                        dstd::Vector<I> lids;
                        dstd::Vector<I> rids;
                        for (std::size_t i : left_order(l, by_mode)) {
                            lids.emplace_back(static_cast<std::int64_t>(i));
                            rids.push_back(
                                asof_ref(l, i, r, type, by_mode, dir, tol));
                        }
                        dstd::Vector<dstd::String> names = left.names;
                        names.emplace_back("rid");

                        const DataFrame out = df::asof(
                            left, right, "t", by_names(by_mode), dir, tol);
                        CHECK(out.names == names);
                        CHECK(i64_col(out, "lid") == lids);
                        CHECK(i64_col(out, "rid") == rids);
                    }
}

TEST_CASE("interval matches a nested-loop reference") {
    std::mt19937 rng(9292026);
    for (TypeId type : TIME_TYPES)
        for (int by_mode = 0; by_mode < 4; ++by_mode)
            for (bool outer : {false, true})
                for (int trial = 0; trial < 40; ++trial) {
                    const std::int64_t base = type == TypeId::Uint64 ? 0 : -6;
                    const Rows l = random_rows(rng, base);
                    const Rows r = random_rows(rng, base);
                    DataFrame left = keyed(l, "lid", by_mode);
                    left.names.emplace_back("p");
                    left.columns.push_back(typed(type, l.t));
                    DataFrame right = keyed(r, "rid", by_mode);
                    right.names.emplace_back("lo");
                    right.columns.push_back(typed(type, r.t));
                    right.names.emplace_back("hi");
                    right.columns.push_back(typed(type, r.hi));
                    INFO("type=" << static_cast<int>(type)
                                 << " by_mode=" << by_mode << " outer=" << outer
                                 << " trial=" << trial);

                    dstd::Vector<I> lids;
                    dstd::Vector<I> rids;
                    for (std::size_t i : left_order(l, by_mode)) {
                        dstd::Vector<std::size_t> hits;
                        for (std::size_t j = 0; l.t[i] && j < r.size(); ++j) {
                            if (!r.t[j] || !r.hi[j] ||
                                !same_keys(l, i, r, j, by_mode))
                                continue;
                            const double x = tval(type, *l.t[i]);
                            if (tval(type, *r.t[j]) <= x &&
                                x <= tval(type, *r.hi[j]))
                                hits.push_back(j);
                        }
                        dstd::stable_sort(hits.begin(), hits.end(),
                                          [&](std::size_t a, std::size_t b) {
                                              if (*r.t[a] != *r.t[b])
                                                  return *r.t[a] < *r.t[b];
                                              return *r.hi[a] < *r.hi[b];
                                          });
                        if (hits.empty() && outer) {
                            lids.emplace_back(static_cast<std::int64_t>(i));
                            rids.push_back(NI);
                        }
                        for (std::size_t j : hits) {
                            lids.emplace_back(static_cast<std::int64_t>(i));
                            rids.emplace_back(static_cast<std::int64_t>(j));
                        }
                    }
                    dstd::Vector<dstd::String> names = left.names;
                    names.emplace_back("rid");

                    const DataFrame out = df::interval(
                        left, right, "p", "lo", "hi", by_names(by_mode), outer);
                    CHECK(out.names == names);
                    CHECK(i64_col(out, "lid") == lids);
                    CHECK(i64_col(out, "rid") == rids);
                }
}
