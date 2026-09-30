#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/frame_ops.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::gap_fill;
using dftracer::utils::dataframe::GapFillMode;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

namespace {

using I = std::optional<std::int64_t>;
using D = std::optional<double>;
using S = std::optional<std::string>;
using Range = std::optional<std::pair<std::int64_t, std::int64_t>>;
const I NI = std::nullopt;
const D ND = std::nullopt;

template <class T>
std::vector<std::uint8_t> validity_of(const std::vector<std::optional<T>>& v) {
    std::vector<std::uint8_t> bits((v.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i]) bits[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
    return bits;
}

template <class T, class U>
Series flat_of(TypeId type, const std::vector<std::optional<U>>& v) {
    std::vector<T> data(v.size(), T{});
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i]) data[i] = static_cast<T>(*v[i]);
    const std::vector<std::uint8_t> bits = validity_of(v);
    return Series::flat(type, data.data(), static_cast<std::int64_t>(v.size()),
                        bits.data());
}

Series i64s(const std::vector<I>& v) {
    return flat_of<std::int64_t>(TypeId::Int64, v);
}

Series f64s(const std::vector<D>& v) {
    return flat_of<double>(TypeId::Float64, v);
}

std::vector<I> int_col(const DataFrame& df, const std::string& name) {
    Series c = df.column(name);
    REQUIRE(c.valid());
    c = c.materialize();
    std::vector<I> out;
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i)) {
            out.emplace_back(std::nullopt);
            continue;
        }
        switch (c.type()) {
            case TypeId::Int32:
                out.emplace_back(c.data<std::int32_t>()[i]);
                break;
            case TypeId::Int64:
                out.emplace_back(c.data<std::int64_t>()[i]);
                break;
            case TypeId::Uint32:
                out.emplace_back(c.data<std::uint32_t>()[i]);
                break;
            default:
                FAIL("unexpected integer column type");
        }
    }
    return out;
}

std::vector<D> f64_col(const DataFrame& df, const std::string& name) {
    Series c = df.column(name);
    REQUIRE(c.valid());
    c = c.materialize();
    REQUIRE(c.type() == TypeId::Float64);
    std::vector<D> out;
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i))
            out.emplace_back(std::nullopt);
        else
            out.emplace_back(c.data<double>()[i]);
    }
    return out;
}

std::vector<S> str_col(const DataFrame& df, const std::string& name) {
    Series c = df.column(name);
    REQUIRE(c.valid());
    c = c.materialize();
    std::vector<S> out;
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i))
            out.emplace_back(std::nullopt);
        else
            out.emplace_back(std::string(c.string_at(i)));
    }
    return out;
}

void check_doubles(const std::vector<D>& got, const std::vector<D>& want) {
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        CAPTURE(i);
        REQUIRE(got[i].has_value() == want[i].has_value());
        if (want[i]) CHECK(*got[i] == doctest::Approx(*want[i]));
    }
}

// {t:Int64, v:Int64, x:Int64}; a nullopt v is a null cell, x = 900 + i.
DataFrame make_tvx(const std::vector<std::int64_t>& t,
                   const std::vector<I>& v) {
    std::vector<I> tt(t.begin(), t.end());
    std::vector<I> x;
    for (std::size_t i = 0; i < t.size(); ++i)
        x.emplace_back(900 + static_cast<std::int64_t>(i));
    DataFrame df;
    df.names = {"t", "v", "x"};
    df.columns.push_back(i64s(tt));
    df.columns.push_back(i64s(v));
    df.columns.push_back(i64s(x));
    return df;
}

// {t:Int64, v:Float64}.
DataFrame make_tvd(const std::vector<std::int64_t>& t,
                   const std::vector<double>& v) {
    DataFrame df;
    df.names = {"t", "v"};
    df.columns.push_back(
        Series::flat_i64(t.data(), static_cast<std::int64_t>(t.size())));
    df.columns.push_back(
        Series::flat_f64(v.data(), static_cast<std::int64_t>(v.size())));
    return df;
}

// {p:Int64, t:Int64, v:Int64}.
DataFrame make_ptv(const std::vector<std::int64_t>& p,
                   const std::vector<std::int64_t>& t,
                   const std::vector<std::int64_t>& v) {
    DataFrame df;
    df.names = {"p", "t", "v"};
    df.columns.push_back(
        Series::flat_i64(p.data(), static_cast<std::int64_t>(p.size())));
    df.columns.push_back(
        Series::flat_i64(t.data(), static_cast<std::int64_t>(t.size())));
    df.columns.push_back(
        Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size())));
    return df;
}

Range range_of(std::int64_t lo, std::int64_t hi) {
    return std::make_pair(lo, hi);
}

}  // namespace

TEST_CASE("gap_fill - None leaves a missing bucket null, preserves real rows") {
    DataFrame out = gap_fill(make_tvx({0, 10, 30}, {100, 200, 400}), {}, "t",
                             10, {"v"}, GapFillMode::None, std::nullopt);
    CHECK(out.names == std::vector<std::string>{"t", "v", "x"});
    CHECK(int_col(out, "t") == std::vector<I>{0, 10, 20, 30});
    CHECK(int_col(out, "v") == std::vector<I>{100, 200, NI, 400});
    CHECK(int_col(out, "x") == std::vector<I>{900, 901, NI, 902});
}

TEST_CASE("gap_fill - Locf carries the last real value forward") {
    DataFrame out = gap_fill(make_tvx({0, 10, 30}, {100, 200, 400}), {}, "t",
                             10, {"v"}, GapFillMode::Locf, std::nullopt);
    CHECK(int_col(out, "v") == std::vector<I>{100, 200, 200, 400});
    CHECK(int_col(out, "x") == std::vector<I>{900, 901, NI, 902});
}

TEST_CASE("gap_fill - Locf leading gap before first real is null") {
    DataFrame out = gap_fill(make_tvx({0, 10}, {100, 200}), {}, "t", 10, {"v"},
                             GapFillMode::Locf, range_of(-20, 10));
    CHECK(int_col(out, "t") == std::vector<I>{-20, -10, 0, 10});
    CHECK(int_col(out, "v") == std::vector<I>{NI, NI, 100, 200});
}

TEST_CASE("gap_fill - Linear interpolates the missing bucket as double") {
    DataFrame out = gap_fill(make_tvd({0, 20}, {100.0, 200.0}), {}, "t", 10,
                             {"v"}, GapFillMode::Linear, std::nullopt);
    CHECK(int_col(out, "t") == std::vector<I>{0, 10, 20});
    check_doubles(f64_col(out, "v"), {100.0, 150.0, 200.0});
}

TEST_CASE("gap_fill - Linear does not extrapolate past the real endpoints") {
    DataFrame out = gap_fill(make_tvd({0, 20}, {100.0, 200.0}), {}, "t", 10,
                             {"v"}, GapFillMode::Linear, range_of(-10, 30));
    CHECK(int_col(out, "t") == std::vector<I>{-10, 0, 10, 20, 30});
    check_doubles(f64_col(out, "v"), {ND, 100.0, 150.0, 200.0, ND});
}

TEST_CASE("gap_fill - partitions are filled independently") {
    DataFrame out =
        gap_fill(make_ptv({1, 1, 2, 2}, {0, 20, 0, 10}, {1, 3, 5, 7}), {"p"},
                 "t", 10, {"v"}, GapFillMode::None, std::nullopt);
    CHECK(int_col(out, "p") == std::vector<I>{1, 1, 1, 2, 2});
    CHECK(int_col(out, "t") == std::vector<I>{0, 10, 20, 0, 10});
    CHECK(int_col(out, "v") == std::vector<I>{1, NI, 3, 5, 7});
}

TEST_CASE("gap_fill - explicit range unifies grids across partitions") {
    DataFrame in = make_ptv({1, 1, 2, 2}, {0, 10, 20, 30}, {1, 2, 3, 4});
    SUBCASE("per-partition min/max keeps each grid tight") {
        DataFrame out = gap_fill(in, {"p"}, "t", 10, {"v"}, GapFillMode::None,
                                 std::nullopt);
        CHECK(out.num_rows() == 4);
    }
    SUBCASE("explicit range gives every partition the same span") {
        DataFrame out = gap_fill(in, {"p"}, "t", 10, {"v"}, GapFillMode::None,
                                 range_of(0, 30));
        CHECK(int_col(out, "p") == std::vector<I>{1, 1, 1, 1, 2, 2, 2, 2});
        CHECK(int_col(out, "t") ==
              std::vector<I>{0, 10, 20, 30, 0, 10, 20, 30});
        CHECK(int_col(out, "v") == std::vector<I>{1, 2, NI, NI, NI, NI, 3, 4});
    }
}

TEST_CASE("gap_fill - deterministic regardless of input row order") {
    DataFrame oa = gap_fill(make_ptv({1, 1, 2}, {0, 20, 10}, {1, 3, 5}), {"p"},
                            "t", 10, {"v"}, GapFillMode::Locf, std::nullopt);
    DataFrame ob = gap_fill(make_ptv({2, 1, 1}, {10, 20, 0}, {5, 3, 1}), {"p"},
                            "t", 10, {"v"}, GapFillMode::Locf, std::nullopt);
    CHECK(int_col(oa, "p") == int_col(ob, "p"));
    CHECK(int_col(oa, "t") == int_col(ob, "t"));
    CHECK(int_col(oa, "v") == int_col(ob, "v"));
    CHECK(int_col(oa, "v") == std::vector<I>{1, 1, 3, 5});
}

TEST_CASE("gap_fill - duplicate real times in one bucket keep the first") {
    DataFrame out = gap_fill(make_tvx({0, 0, 20}, {100, 111, 200}), {}, "t", 10,
                             {"v"}, GapFillMode::None, std::nullopt);
    CHECK(int_col(out, "t") == std::vector<I>{0, 10, 20});
    CHECK(int_col(out, "v") == std::vector<I>{100, NI, 200});
}

TEST_CASE("gap_fill - rejects a non-positive bucket width") {
    DataFrame in = make_tvx({0, 10}, {1, 2});
    CHECK_THROWS_AS(
        gap_fill(in, {}, "t", 0, {"v"}, GapFillMode::None, std::nullopt),
        std::invalid_argument);
    CHECK_THROWS_AS(
        gap_fill(in, {}, "t", -10, {"v"}, GapFillMode::None, std::nullopt),
        std::invalid_argument);
}

TEST_CASE("gap_fill - runaway grid guard trips on a huge range") {
    CHECK_THROWS_AS(gap_fill(make_tvx({0, 10}, {1, 2}), {}, "t", 1, {"v"},
                             GapFillMode::None, range_of(0, 1'000'000'000)),
                    std::invalid_argument);
}

TEST_CASE("gap_fill - grid guard trips before the span overflows int64") {
    const std::int64_t lo = std::numeric_limits<std::int64_t>::min();
    const std::int64_t hi = std::numeric_limits<std::int64_t>::max();
    CHECK_THROWS_AS(gap_fill(make_tvx({0, 10}, {1, 2}), {}, "t", 1, {"v"},
                             GapFillMode::None, range_of(lo, hi)),
                    std::invalid_argument);
}

TEST_CASE("gap_fill - rejects unknown columns and unsupported types") {
    DataFrame in = make_tvx({0, 10}, {1, 2});
    CHECK_THROWS_AS(
        gap_fill(in, {}, "nope", 10, {"v"}, GapFillMode::None, std::nullopt),
        std::out_of_range);
    CHECK_THROWS_AS(
        gap_fill(in, {"nope"}, "t", 10, {"v"}, GapFillMode::None, std::nullopt),
        std::out_of_range);
    CHECK_THROWS_AS(
        gap_fill(in, {}, "t", 10, {"nope"}, GapFillMode::None, std::nullopt),
        std::out_of_range);
    CHECK_THROWS_AS(gap_fill(make_tvd({0, 10}, {1.0, 2.0}), {}, "v", 10, {},
                             GapFillMode::None, std::nullopt),
                    std::invalid_argument);
    DataFrame s = make_tvx({0, 10}, {1, 2});
    s.names.push_back("s");
    s.columns.push_back(Series::strings(std::vector<std::string>{"a", "b"}));
    CHECK_THROWS_AS(
        gap_fill(s, {}, "t", 10, {"s"}, GapFillMode::Linear, std::nullopt),
        std::invalid_argument);
}

namespace {

struct Row {
    I p;
    std::string s;
    I t;
    I v;
    D w;
    std::int64_t x;
};

struct Out {
    I p;
    std::string s;
    std::int64_t t;
    D v;
    D w;
    I x;
};

std::int64_t floor_div(std::int64_t t, std::int64_t w) {
    return t >= 0 ? t / w : -((-t + w - 1) / w);
}

D interpolate(D v0, D v1, std::int64_t t0, std::int64_t t1, std::int64_t g) {
    if (!v0 || !v1) return std::nullopt;
    return *v0 + (*v1 - *v0) * static_cast<double>(g - t0) /
                     static_cast<double>(t1 - t0);
}

std::vector<Out> reference(const std::vector<Row>& rows, std::int64_t width,
                           GapFillMode mode, Range range) {
    const auto key_less = [&](std::size_t a, std::size_t b) {
        const Row& x = rows[a];
        const Row& y = rows[b];
        if (x.p.has_value() != y.p.has_value()) return x.p.has_value();
        if (x.p && *x.p != *y.p) return *x.p < *y.p;
        return x.s < y.s;
    };
    std::vector<std::size_t> idx(rows.size());
    std::iota(idx.begin(), idx.end(), std::size_t{0});
    std::stable_sort(idx.begin(), idx.end(), key_less);

    const auto as_d = [](I v) -> D {
        return v ? D(static_cast<double>(*v)) : std::nullopt;
    };
    std::vector<Out> out;
    for (std::size_t a = 0; a < idx.size();) {
        std::size_t b = a + 1;
        while (b < idx.size() && !key_less(idx[a], idx[b])) ++b;
        std::map<std::int64_t, std::size_t> first;
        for (std::size_t r = a; r < b; ++r) {
            const Row& row = rows[idx[r]];
            if (!row.t) continue;
            const std::int64_t k = floor_div(*row.t, width);
            auto it = first.find(k);
            if (it == first.end() || *row.t < *rows[it->second].t)
                first[k] = idx[r];
        }
        const Row& key = rows[idx[a]];
        a = b;
        if (first.empty()) continue;
        const std::int64_t start =
            range ? floor_div(range->first, width) : first.begin()->first;
        const std::int64_t end =
            range ? floor_div(range->second, width) : first.rbegin()->first;
        for (std::int64_t k = start; k <= end; ++k) {
            Out o{key.p,        key.s,        k * width,
                  std::nullopt, std::nullopt, std::nullopt};
            const auto it = first.find(k);
            if (it != first.end()) {
                const Row& r = rows[it->second];
                o.t = *r.t;
                o.v = as_d(r.v);
                o.w = r.w;
                o.x = r.x;
                out.push_back(o);
                continue;
            }
            auto prev = first.lower_bound(k);
            const bool has_prev = prev != first.begin();
            if (has_prev) --prev;
            const auto next = first.upper_bound(k);
            if (mode == GapFillMode::Locf && has_prev) {
                o.v = as_d(rows[prev->second].v);
                o.w = rows[prev->second].w;
            } else if (mode == GapFillMode::Linear && has_prev &&
                       next != first.end()) {
                const Row& lo = rows[prev->second];
                const Row& hi = rows[next->second];
                o.v = interpolate(as_d(lo.v), as_d(hi.v), *lo.t, *hi.t,
                                  k * width);
                o.w = interpolate(lo.w, hi.w, *lo.t, *hi.t, k * width);
            }
            out.push_back(o);
        }
    }
    return out;
}

Series time_series(TypeId type, const std::vector<I>& t) {
    switch (type) {
        case TypeId::Int32:
            return flat_of<std::int32_t>(type, t);
        case TypeId::Uint32:
            return flat_of<std::uint32_t>(type, t);
        default:
            return flat_of<std::int64_t>(type, t);
    }
}

}  // namespace

TEST_CASE("gap_fill - randomized frames match a naive reference") {
    std::mt19937_64 rng(0x6a9f11);
    const auto uniform = [&](std::int64_t lo, std::int64_t hi) {
        return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
    };
    const auto chance = [&](int percent) { return uniform(0, 99) < percent; };
    const TypeId TIME_TYPES[] = {TypeId::Int32, TypeId::Int64, TypeId::Uint32};
    const std::int64_t WIDTHS[] = {1, 2, 5, 10};
    const std::string STRINGS[] = {"", "a", "b"};
    const GapFillMode MODES[] = {GapFillMode::None, GapFillMode::Locf,
                                 GapFillMode::Linear};

    for (int trial = 0; trial < 300; ++trial) {
        const TypeId time_type = TIME_TYPES[uniform(0, 2)];
        const bool is_unsigned = time_type == TypeId::Uint32;
        const std::int64_t width = WIDTHS[uniform(0, 3)];
        const auto n = static_cast<std::size_t>(uniform(0, 30));
        std::vector<Row> rows;
        for (std::size_t i = 0; i < n; ++i) {
            Row r;
            r.p = chance(10) ? NI : I(uniform(1, 3));
            r.s = STRINGS[uniform(0, 2)];
            r.t = chance(10)
                      ? NI
                      : I(is_unsigned ? uniform(0, 80) : uniform(-40, 40));
            r.v = chance(15) ? NI : I(uniform(-100, 100));
            r.w = chance(15) ? ND
                             : D(static_cast<double>(uniform(-100, 100)) / 4);
            r.x = 900 + static_cast<std::int64_t>(i);
            rows.push_back(r);
        }

        std::vector<I> p, t, v, x;
        std::vector<D> w;
        std::vector<std::string> s;
        for (const Row& r : rows) {
            p.push_back(r.p);
            s.push_back(r.s);
            t.push_back(r.t);
            v.push_back(r.v);
            w.push_back(r.w);
            x.push_back(r.x);
        }
        DataFrame df;
        df.names = {"p", "s", "t", "v", "w", "x"};
        df.columns.push_back(i64s(p));
        df.columns.push_back(Series::strings(s));
        df.columns.push_back(time_series(time_type, t));
        df.columns.push_back(i64s(v));
        df.columns.push_back(f64s(w));
        df.columns.push_back(i64s(x));

        for (const GapFillMode mode : MODES) {
            for (int ranged = 0; ranged < 2; ++ranged) {
                Range range;
                if (ranged) {
                    const std::int64_t lo =
                        is_unsigned ? uniform(0, 40) : uniform(-50, 30);
                    range = range_of(lo, lo + uniform(-10, 60));
                }
                CAPTURE(trial);
                CAPTURE(static_cast<int>(mode));
                CAPTURE(ranged);
                const std::vector<Out> want =
                    reference(rows, width, mode, range);
                DataFrame out = gap_fill(df, {"p", "s"}, "t", width, {"v", "w"},
                                         mode, range);
                REQUIRE(out.names == df.names);
                REQUIRE(out.num_rows() ==
                        static_cast<std::int64_t>(want.size()));
                CHECK(out.column("t").type() == time_type);

                const std::vector<I> op = int_col(out, "p");
                const std::vector<S> os = str_col(out, "s");
                const std::vector<I> ot = int_col(out, "t");
                const std::vector<I> ox = int_col(out, "x");
                std::vector<D> ov;
                if (mode == GapFillMode::Linear) {
                    ov = f64_col(out, "v");
                } else {
                    REQUIRE(out.column("v").type() == TypeId::Int64);
                    for (const I c : int_col(out, "v"))
                        ov.push_back(c ? D(static_cast<double>(*c)) : ND);
                }
                std::vector<D> want_v, want_w;
                for (std::size_t i = 0; i < want.size(); ++i) {
                    CAPTURE(i);
                    CHECK(op[i] == want[i].p);
                    CHECK(os[i] == S(want[i].s));
                    CHECK(ot[i] == I(want[i].t));
                    CHECK(ox[i] == want[i].x);
                    want_v.push_back(want[i].v);
                    want_w.push_back(want[i].w);
                }
                check_doubles(ov, want_v);
                check_doubles(f64_col(out, "w"), want_w);
            }
        }
    }
}
