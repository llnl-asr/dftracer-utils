// The SketchQuantile group-by op merges base64 DDSketch cells per group and
// reads one quantile; DDSketch::deserialize refuses hostile bytes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/core/common/base64.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace df = dftracer::utils::dataframe;
using df::Agg;
using df::AggOp;
using df::AggSpec;
using df::DataFrame;
using df::DDSketch;
using df::GroupAgg;
using df::LazyFrame;
using df::Series;
using df::TypeId;

namespace {

const std::vector<double> QUANTILES{0.0, 0.01, 0.5, 0.99, 1.0};

DDSketch sketch_of(const std::vector<double>& values, double accuracy = 0.01) {
    DDSketch s(accuracy);
    for (double v : values) s.add(v);
    return s;
}

std::string text_of(const DDSketch& s) {
    const auto bytes = s.serialize();
    return dftracer::utils::base64_encode(bytes.data(), bytes.size());
}

double clamped(const DDSketch& s, double q) {
    return std::clamp(s.quantile(q), s.min(), s.max());
}

struct Cell {
    std::int64_t key;
    std::optional<std::string> text;
};

DataFrame frame_of(const std::vector<Cell>& cells) {
    std::vector<std::int64_t> keys;
    std::vector<std::string_view> views;
    std::vector<std::uint8_t> valid((cells.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        keys.push_back(cells[i].key);
        views.push_back(cells[i].text ? std::string_view(*cells[i].text)
                                      : std::string_view());
        if (cells[i].text) valid[i >> 3] |= std::uint8_t(1u << (i & 7));
    }
    DataFrame d;
    d.names = {"k", "s"};
    d.columns.push_back(
        Series::flat_i64(keys.data(), static_cast<std::int64_t>(keys.size())));
    d.columns.push_back(Series::strings(views, valid.data()));
    return d;
}

std::vector<GroupAgg> quantile_aggs() {
    std::vector<GroupAgg> aggs;
    for (double q : QUANTILES)
        aggs.push_back(
            GroupAgg{Agg::SketchQuantile, "s", "q" + std::to_string(q), q});
    return aggs;
}

// The same text with 64-bit offsets, as a column of more than 2 GB holds it.
Series large_copy(const Series& s) {
    auto* w = new dftu_series();
    w->type = TypeId::LargeString;
    w->encoding = df::Encoding::Flat;
    w->length = s.length();
    const auto n = static_cast<std::size_t>(s.length());
    w->offsets = df::Buffer::allocate((n + 1) * sizeof(std::int64_t));
    auto* off = reinterpret_cast<std::int64_t*>(w->offsets->data());
    std::string bytes;
    std::vector<std::uint8_t> valid((n + 7) / 8, 0);
    off[0] = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto r = static_cast<std::int64_t>(i);
        if (s.is_null(r)) {
            ++w->null_count;
        } else {
            bytes += s.string_at(r);
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        }
        off[i + 1] = static_cast<std::int64_t>(bytes.size());
    }
    w->data = df::Buffer::allocate(bytes.size());
    if (!bytes.empty())
        std::memcpy(w->data->data(), bytes.data(), bytes.size());
    if (w->null_count > 0) {
        w->validity = df::Buffer::allocate(valid.size());
        std::memcpy(w->validity->data(), valid.data(), valid.size());
    }
    return Series{w};
}

std::int64_t row_of(const DataFrame& d, std::int64_t key) {
    const Series k = d.column("k").materialize();
    for (std::int64_t i = 0; i < d.num_rows(); ++i)
        if (k.data<std::int64_t>()[i] == key) return i;
    return -1;
}

DataFrame run(dftracer::utils::coro::CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

LazyFrame scan(const DataFrame& d) {
    DataFrame copy;
    copy.names = d.names;
    for (const Series& c : d.columns) copy.columns.push_back(c.share());
    return LazyFrame::scan(
        std::make_shared<df::InMemorySource>(std::move(copy)));
}

// Group 0 and 1 hold valid sketches only. Group 2 mixes valid cells with a
// null, bad base64, an empty sketch and a sketch of another accuracy. Group 3
// holds nothing valid.
struct Fixture {
    std::vector<double> g0a{1, 2, 3, 4, 5};
    std::vector<double> g0b{6, 7, 8, 9, 0};
    std::vector<double> g1a{2.5, 2.5, 2.5};
    std::vector<double> g2a{3, 4, 5};
    std::vector<double> g2b{6, 6, 7};
    DataFrame frame;

    Fixture() {
        frame = frame_of({
            {0, text_of(sketch_of(g0a))},
            {1, text_of(sketch_of(g1a))},
            {0, text_of(sketch_of(g0b))},
            {2, text_of(sketch_of(g2a))},
            {2, std::nullopt},
            {2, std::string("not base64!")},
            {2, text_of(DDSketch(0.01))},
            {2, text_of(sketch_of({100, 200}, 0.05))},
            {2, text_of(sketch_of(g2b))},
            {3, std::nullopt},
            {3, std::string("###")},
            {3, text_of(DDSketch(0.01))},
            {3, std::string("AAAA")},
        });
    }

    static DDSketch concat(const std::vector<double>& a,
                           const std::vector<double>& b) {
        std::vector<double> all = a;
        all.insert(all.end(), b.begin(), b.end());
        return sketch_of(all);
    }

    void check(const DataFrame& r) const {
        REQUIRE(r.num_rows() == 4);
        const std::int64_t rows[] = {row_of(r, 0), row_of(r, 1), row_of(r, 2),
                                     row_of(r, 3)};
        const DDSketch want[] = {concat(g0a, g0b), sketch_of(g1a),
                                 concat(g2a, g2b)};
        for (std::size_t qi = 0; qi < QUANTILES.size(); ++qi) {
            const Series c =
                r.column("q" + std::to_string(QUANTILES[qi])).materialize();
            REQUIRE(c.type() == TypeId::Float64);
            for (int g = 0; g < 3; ++g) {
                REQUIRE_FALSE(c.is_null(rows[g]));
                CHECK(c.data<double>()[rows[g]] ==
                      clamped(want[g], QUANTILES[qi]));
            }
            CHECK(c.is_null(rows[3]));
        }
    }
};

}  // namespace

TEST_CASE("sketch_quantile: merged cells equal one sketch over the group") {
    const DDSketch g0 = Fixture::concat({1, 2, 3, 4, 5}, {6, 7, 8, 9, 0});
    const DataFrame d = frame_of({
        {0, text_of(sketch_of({1, 2, 3, 4, 5}))},
        {0, text_of(sketch_of({6, 7, 8, 9, 0}))},
    });
    const DataFrame r = d.group_by("k", quantile_aggs());
    REQUIRE(r.num_rows() == 1);
    for (double q : QUANTILES)
        CHECK(r.column("q" + std::to_string(q)).data<double>()[0] ==
              clamped(g0, q));
}

TEST_CASE(
    "sketch_quantile: invalid cells are skipped, an empty group is null") {
    Fixture f;
    f.check(f.frame.group_by("k", quantile_aggs()));
}

TEST_CASE("sketch_quantile: reads large string sketch text") {
    Fixture f;
    DataFrame large;
    large.names = f.frame.names;
    large.columns.push_back(f.frame.column("k").share());
    large.columns.push_back(large_copy(f.frame.column("s")));
    REQUIRE(large.column("s").type() == TypeId::LargeString);
    f.check(large.group_by("k", quantile_aggs()));
}

TEST_CASE("sketch_quantile: lazy and spilled group_by match eager") {
    Fixture f;
    const std::vector<std::string> keys{"k"};
    for (std::int64_t morsel : std::vector<std::int64_t>{0, 1, 3}) {
        f.check(run(scan(f.frame)
                        .group_by(keys, quantile_aggs())
                        .sort_by("k")
                        .collect(morsel)));
    }
    f.check(run(scan(f.frame)
                    .memory_budget(1)
                    .group_by(keys, quantile_aggs())
                    .sort_by("k")
                    .collect(1)));
}

TEST_CASE("sketch_quantile: the agg state merges and round-trips") {
    Fixture f;
    const std::vector<AggSpec> specs{
        AggSpec{AggOp::SketchQuantile, 0, "p50", 0.5},
        AggSpec{AggOp::SketchQuantile, 0, "p99", 0.99},
    };
    const Series k = f.frame.column("k");
    const Series s = f.frame.column("s");
    const std::int64_t n = f.frame.num_rows();
    const std::int64_t half = n / 2;
    auto a = df::agg_new(specs);
    auto b = df::agg_new(specs);
    const std::vector<const Series*> keys{&k};
    const std::vector<const Series*> values{&s};
    df::agg_accumulate(*a, keys, values, 0, half);
    df::agg_accumulate(*b, keys, values, half, n);
    auto b2 = df::agg_deserialize(df::agg_serialize(*b));
    REQUIRE(b2);
    df::agg_merge(*a, *b2);
    df::agg_sort_groups(*a);

    auto whole = df::agg_new(specs);
    df::agg_accumulate(*whole, keys, values);
    df::agg_sort_groups(*whole);

    const DataFrame got = df::agg_finalize(*a, "k");
    const DataFrame want = df::agg_finalize(*whole, "k");
    REQUIRE(got.num_rows() == 4);
    REQUIRE(want.num_rows() == 4);
    for (const char* name : {"p50", "p99"}) {
        const Series gc = got.column(name);
        const Series wc = want.column(name);
        for (std::int64_t i = 0; i < 4; ++i) {
            REQUIRE(gc.is_null(i) == wc.is_null(i));
            if (!gc.is_null(i))
                CHECK(gc.data<double>()[i] == wc.data<double>()[i]);
        }
    }
}

TEST_CASE("sketch_quantile: a non-String column is refused") {
    std::vector<std::int64_t> k{0, 0};
    std::vector<std::int64_t> v{1, 2};
    DataFrame d;
    d.names = {"k", "s"};
    d.columns.push_back(Series::flat_i64(k.data(), 2));
    d.columns.push_back(Series::flat_i64(v.data(), 2));
    CHECK_THROWS_AS(d.group_by("k", quantile_aggs()), std::invalid_argument);
}

TEST_CASE("DDSketch::deserialize rejects truncated and hostile bytes") {
    const DDSketch good = sketch_of({1, 2, 3, 4, 5});
    const std::vector<std::uint8_t> bytes = good.serialize();
    REQUIRE_FALSE(DDSketch::deserialize(bytes.data(), bytes.size()).empty());

    for (std::size_t len = 0; len < bytes.size(); ++len)
        CHECK(DDSketch::deserialize(bytes.data(), len).empty());

    auto poke = [&](std::size_t at, const void* v, std::size_t n) {
        std::vector<std::uint8_t> m = bytes;
        std::memcpy(m.data() + at, v, n);
        return DDSketch::deserialize(m.data(), m.size());
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double one = 1.0;
    CHECK(poke(0, &nan, sizeof nan).empty());
    CHECK(poke(0, &inf, sizeof inf).empty());
    CHECK(poke(0, &one, sizeof one).empty());
    // The offset sits after gamma, min, max, count and zero_count.
    constexpr std::size_t OFFSET_AT = 3 * sizeof(double) + 2 * 8;
    for (std::int32_t off : {std::numeric_limits<std::int32_t>::max(),
                             std::numeric_limits<std::int32_t>::min()})
        CHECK(poke(OFFSET_AT, &off, sizeof off).empty());

    // A bin count past the blob is cut to what the bytes hold or refused.
    const std::uint32_t huge = 0xffffffffu;
    CHECK(poke(OFFSET_AT + 4, &huge, sizeof huge).empty());
}

TEST_CASE("DDSketch survives random mutations of a serialized sketch") {
    const std::vector<std::uint8_t> bytes =
        sketch_of({0.5, 1, 2, 3, 40, 500, 7000}).serialize();
    std::uint64_t x = 0x9e3779b97f4a7c15ULL;
    auto next = [&]() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    for (int round = 0; round < 5000; ++round) {
        std::vector<std::uint8_t> m = bytes;
        const int edits = 1 + static_cast<int>(next() % 6);
        for (int e = 0; e < edits; ++e)
            m[next() % m.size()] = static_cast<std::uint8_t>(next() & 0xff);
        if (round % 7 == 0) m.resize(next() % m.size());
        DDSketch s = DDSketch::deserialize(m.data(), m.size());
        DDSketch into = sketch_of({1, 2, 3});
        into.merge(s);
        s.merge(into);
        (void)s.quantile(0.5);
        (void)s.bins();
        (void)s.serialize();
    }
}
