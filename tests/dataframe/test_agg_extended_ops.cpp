#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace df = dftracer::utils::dataframe;
using df::AggOp;
using df::AggSpec;
using df::DataFrame;
using df::Series;
using df::TypeId;

namespace {

// Two groups (k = 0 rows 0,2,4,6; k = 1 rows 1,3,5,7) over a `by` ordering
// column, a name/alt pair of String value columns, a one-hot bit column, a
// repeating tag column and y = 2*by + 1 (an exact linear fit against `by`).
struct Sample {
    std::vector<std::int64_t> k{0, 1, 0, 1, 0, 1, 0, 1};
    std::vector<std::int64_t> by{5, 1, 2, 8, 9, 3, 4, 7};
    std::vector<std::int64_t> bits{1, 2, 4, 8, 16, 32, 64, 128};
    std::vector<std::int64_t> yv{11, 3, 5, 17, 19, 7, 9, 15};
    std::vector<std::string> name{"a", "b", "c", "d", "e", "f", "g", "h"};
    std::vector<std::string> alt{"A", "B", "C", "D", "E", "F", "G", "H"};
    std::vector<std::string> tag{"x", "x", "x", "y", "y", "z", "x", "w"};
    Series sk, sby, sbits, syv, sname, salt, stag;
    std::vector<const Series*> keys, values;

    Sample() {
        sk = Series::flat_i64(k.data(), 8);
        sby = Series::flat_i64(by.data(), 8);
        sbits = Series::flat_i64(bits.data(), 8);
        syv = Series::flat_i64(yv.data(), 8);
        sname = Series::strings(name);
        salt = Series::strings(alt);
        stag = Series::strings(tag);
        keys = {&sk};
        values = {&sname, &sby, &sbits, &stag, &syv, &salt};
    }
};

// value_col indices into Sample::values.
constexpr std::int32_t VC_NAME = 0;
constexpr std::int32_t VC_BY = 1;
constexpr std::int32_t VC_BITS = 2;
constexpr std::int32_t VC_TAG = 3;
constexpr std::int32_t VC_YV = 4;

std::vector<AggSpec> all_specs() {
    return {
        AggSpec{AggOp::ArgMin, VC_NAME, "argmin", 0.0, VC_BY},
        AggSpec{AggOp::BitOr, VC_BITS, "bit_or"},
        AggSpec{AggOp::Distinct, VC_NAME, "distinct"},
        AggSpec{AggOp::ListSorted, VC_NAME, "list_sorted", 0.0, VC_BY},
        AggSpec{AggOp::TopK, VC_NAME, "topk", 2.0, VC_BY},
        AggSpec{AggOp::BottomK, VC_NAME, "bottomk", 2.0, VC_BY},
        AggSpec{AggOp::ApproxTopK, VC_TAG, "approx_topk", 4.0},
        AggSpec{AggOp::Sample, VC_TAG, "sample", 8.0},
        AggSpec{AggOp::Corr, VC_YV, "corr", 0.0, VC_BY},
        AggSpec{AggOp::CovarPop, VC_YV, "covar_pop", 0.0, VC_BY},
        AggSpec{AggOp::CovarSamp, VC_YV, "covar_samp", 0.0, VC_BY},
        AggSpec{AggOp::RegrSlope, VC_YV, "regr_slope", 0.0, VC_BY},
        AggSpec{AggOp::RegrIntercept, VC_YV, "regr_intercept", 0.0, VC_BY},
        AggSpec{AggOp::RegrR2, VC_YV, "regr_r2", 0.0, VC_BY},
        AggSpec{AggOp::Prod, VC_BY, "prod"},
    };
}

// One cell as text, so a nested list/struct result is a single comparable
// value. Strings are verbatim, a list is "[a,b]", a struct "{a,b}".
std::string render(const Series& c, std::int64_t i) {
    switch (c.type()) {
        case TypeId::String:
            return std::string(c.string_at(i));
        case TypeId::List: {
            const std::int32_t* off = c.offsets();
            const Series vals = c.child(0);
            std::string out = "[";
            for (std::int32_t j = off[i]; j < off[i + 1]; ++j) {
                if (j > off[i]) out += ",";
                out += render(vals, j);
            }
            return out + "]";
        }
        case TypeId::Struct: {
            std::string out = "{";
            for (std::int64_t f = 0; f < c.num_children(); ++f) {
                if (f) out += ",";
                out += render(c.child(f), i);
            }
            return out + "}";
        }
        case TypeId::Uint64:
            return std::to_string(c.data<std::uint64_t>()[i]);
        case TypeId::Float64:
            return std::to_string(c.data<double>()[i]);
        default:
            return std::to_string(c.data<std::int64_t>()[i]);
    }
}

// Row index of key value `key` in a finalized frame keyed on one Int64 column.
std::int64_t row_of(const DataFrame& d, const std::string& key_name,
                    std::int64_t key) {
    const Series c = d.column(key_name);
    for (std::int64_t i = 0; i < d.num_rows(); ++i)
        if (c.data<std::int64_t>()[i] == key) return i;
    return -1;
}

void check_same(const DataFrame& a, const DataFrame& b) {
    REQUIRE(a.names == b.names);
    REQUIRE(a.num_rows() == b.num_rows());
    for (std::size_t c = 0; c < a.names.size(); ++c)
        for (std::int64_t r = 0; r < a.num_rows(); ++r)
            CHECK(render(a.columns[c], r) == render(b.columns[c], r));
}

}  // namespace

TEST_CASE("extended aggregate ops produce the exact expected values") {
    Sample s;
    DataFrame r = df::group_agg(s.keys, s.values, all_specs(),
                                std::vector<std::string>{"k"});
    REQUIRE(r.num_rows() == 2);
    const std::int64_t g0 = row_of(r, "k", 0);
    const std::int64_t g1 = row_of(r, "k", 1);
    REQUIRE(g0 >= 0);
    REQUIRE(g1 >= 0);

    CHECK(std::string(r.column("argmin").string_at(g0)) == "c");
    CHECK(std::string(r.column("argmin").string_at(g1)) == "b");

    REQUIRE(r.column("bit_or").type() == TypeId::Uint64);
    CHECK(r.column("bit_or").data<std::uint64_t>()[g0] == 85u);
    CHECK(r.column("bit_or").data<std::uint64_t>()[g1] == 170u);

    REQUIRE(r.column("prod").type() == TypeId::Float64);
    CHECK(r.column("prod").data<double>()[g0] == doctest::Approx(360.0));
    CHECK(r.column("prod").data<double>()[g1] == doctest::Approx(168.0));

    // 4 distinct names per group, below the default k, so the KMV count is
    // exact.
    REQUIRE(r.column("distinct").type() == TypeId::Int64);
    CHECK(r.column("distinct").data<std::int64_t>()[g0] == 4);
    CHECK(r.column("distinct").data<std::int64_t>()[g1] == 4);

    CHECK(render(r.column("list_sorted"), g0) == "[c,g,a,e]");
    CHECK(render(r.column("list_sorted"), g1) == "[b,f,h,d]");

    CHECK(render(r.column("topk"), g0) == "[e,a]");
    CHECK(render(r.column("topk"), g1) == "[d,h]");

    CHECK(render(r.column("bottomk"), g0) == "[c,g]");
    CHECK(render(r.column("bottomk"), g1) == "[b,f]");

    CHECK(render(r.column("approx_topk"), g0) == "[{x,3},{y,1}]");
    CHECK(render(r.column("approx_topk"), g1) == "[{w,1},{x,1},{y,1},{z,1}]");

    CHECK(render(r.column("sample"), g0) == "[x,y]");
    CHECK(render(r.column("sample"), g1) == "[w,x,y,z]");

    CHECK(r.column("corr").data<double>()[g0] == doctest::Approx(1.0));
    CHECK(r.column("corr").data<double>()[g1] == doctest::Approx(1.0));
    CHECK(r.column("covar_pop").data<double>()[g0] == doctest::Approx(13.0));
    CHECK(r.column("covar_pop").data<double>()[g1] == doctest::Approx(16.375));
    CHECK(r.column("covar_samp").data<double>()[g0] ==
          doctest::Approx(52.0 / 3.0));
    CHECK(r.column("covar_samp").data<double>()[g1] ==
          doctest::Approx(65.5 / 3.0));
    CHECK(r.column("regr_slope").data<double>()[g0] == doctest::Approx(2.0));
    CHECK(r.column("regr_slope").data<double>()[g1] == doctest::Approx(2.0));
    CHECK(r.column("regr_intercept").data<double>()[g0] ==
          doctest::Approx(1.0));
    CHECK(r.column("regr_intercept").data<double>()[g1] ==
          doctest::Approx(1.0));
    CHECK(r.column("regr_r2").data<double>()[g0] == doctest::Approx(1.0));
    CHECK(r.column("regr_r2").data<double>()[g1] == doctest::Approx(1.0));
}

TEST_CASE("extended aggregate ops round-trip through serialize/deserialize") {
    Sample s;
    auto st = df::agg_new(all_specs());
    df::agg_accumulate(*st, s.keys, s.values);
    const DataFrame direct = df::agg_finalize(*st, "k");
    auto back = df::agg_deserialize(df::agg_serialize(*st));
    check_same(direct, df::agg_finalize(*back, "k"));
}

TEST_CASE(
    "extended aggregate ops merge two partials into the one-pass result") {
    Sample s;
    auto whole = df::agg_new(all_specs());
    df::agg_accumulate(*whole, s.keys, s.values);

    auto lo = df::agg_new(all_specs());
    df::agg_accumulate(*lo, s.keys, s.values, 0, 4);
    auto hi = df::agg_new(all_specs());
    df::agg_accumulate(*hi, s.keys, s.values, 4, 8);
    df::agg_merge(*lo, *hi);

    check_same(df::agg_finalize(*whole, "k"), df::agg_finalize(*lo, "k"));

    // Merging in the other order must land on the same state.
    auto hi2 = df::agg_new(all_specs());
    df::agg_accumulate(*hi2, s.keys, s.values, 4, 8);
    auto lo2 = df::agg_new(all_specs());
    df::agg_accumulate(*lo2, s.keys, s.values, 0, 4);
    df::agg_merge(*hi2, *lo2);
    check_same(df::agg_finalize(*whole, "k"), df::agg_finalize(*hi2, "k"));
}

// Two ArgMin specs naming one `by` column share a slot, so both report the
// value at the SAME winning row: `min_alt` must be the alt of the row that won
// on `by`, not its own smallest repr. A tied `by` keeps the row seen first.
TEST_CASE("ArgMin specs sharing a by column return one whole row") {
    std::vector<std::int64_t> k{0, 0, 0};
    std::vector<std::int64_t> by{3, 1, 2};
    std::vector<std::string> name{"b", "a", "c"};
    std::vector<std::string> alt{"B", "Z", "A"};
    Series sk = Series::flat_i64(k.data(), 3);
    Series sby = Series::flat_i64(by.data(), 3);
    Series sname = Series::strings(name);
    Series salt = Series::strings(alt);
    const std::vector<const Series*> keys{&sk};
    const std::vector<const Series*> values{&sname, &sby, &salt};
    const std::vector<AggSpec> specs{
        AggSpec{AggOp::ArgMin, 0, "min_name", 0.0, 1},
        AggSpec{AggOp::ArgMin, 2, "min_alt", 0.0, 1},
    };

    auto whole = df::agg_new(specs);
    df::agg_accumulate(*whole, keys, values);
    DataFrame r = df::agg_finalize(*whole, "k");
    REQUIRE(r.num_rows() == 1);
    CHECK(std::string(r.column("min_name").string_at(0)) == "a");
    CHECK(std::string(r.column("min_alt").string_at(0)) == "Z");

    // The shared row survives a split accumulate + merge and a round-trip.
    auto lo = df::agg_new(specs);
    df::agg_accumulate(*lo, keys, values, 0, 1);
    auto hi = df::agg_new(specs);
    df::agg_accumulate(*hi, keys, values, 1, 3);
    df::agg_merge(*lo, *hi);
    check_same(r, df::agg_finalize(*lo, "k"));
    check_same(
        r, df::agg_finalize(*df::agg_deserialize(df::agg_serialize(*lo)), "k"));
}

// A tied `by` keeps the row seen first, matching the View fold the two engines
// are compared against; both specs still report that one row.
TEST_CASE("ArgMin breaks a by tie on the first row seen") {
    std::vector<std::int64_t> k{0, 0, 0};
    std::vector<std::int64_t> by{1, 1, 1};
    std::vector<std::string> name{"b", "a", "c"};
    std::vector<std::string> alt{"B", "Z", "A"};
    Series sk = Series::flat_i64(k.data(), 3);
    Series sby = Series::flat_i64(by.data(), 3);
    Series sname = Series::strings(name);
    Series salt = Series::strings(alt);
    const std::vector<const Series*> keys{&sk};
    const std::vector<const Series*> values{&sname, &sby, &salt};
    const std::vector<AggSpec> specs{
        AggSpec{AggOp::ArgMin, 0, "min_name", 0.0, 1},
        AggSpec{AggOp::ArgMin, 2, "min_alt", 0.0, 1},
    };

    auto st = df::agg_new(specs);
    df::agg_accumulate(*st, keys, values);
    DataFrame r = df::agg_finalize(*st, "k");
    REQUIRE(r.num_rows() == 1);
    CHECK(std::string(r.column("min_name").string_at(0)) == "b");
    CHECK(std::string(r.column("min_alt").string_at(0)) == "B");
}

// ArgMin and ArgMax on one `by` column are separate slots (different
// directions) and must not collide.
TEST_CASE("ArgMin and ArgMax on one by column keep separate extremes") {
    Sample s;
    const std::vector<AggSpec> specs{
        AggSpec{AggOp::ArgMin, VC_NAME, "lo", 0.0, VC_BY},
        AggSpec{AggOp::ArgMax, VC_NAME, "hi", 0.0, VC_BY},
    };
    DataFrame r =
        df::group_agg(s.keys, s.values, specs, std::vector<std::string>{"k"});
    const std::int64_t g0 = row_of(r, "k", 0);
    const std::int64_t g1 = row_of(r, "k", 1);
    CHECK(std::string(r.column("lo").string_at(g0)) == "c");
    CHECK(std::string(r.column("hi").string_at(g0)) == "e");
    CHECK(std::string(r.column("lo").string_at(g1)) == "b");
    CHECK(std::string(r.column("hi").string_at(g1)) == "d");
}

// Past k distinct values Distinct is the KMV estimate, not a count: it must
// stay in the right ballpark and Sample must keep exactly k values.
TEST_CASE("Distinct estimates and Sample bounds beyond k") {
    const std::int64_t n = 2000;
    std::vector<std::int64_t> k(static_cast<std::size_t>(n), 0);
    std::vector<std::string> v;
    v.reserve(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) v.push_back("v" + std::to_string(i));
    Series sk = Series::flat_i64(k.data(), n);
    Series sv = Series::strings(v);
    const std::vector<const Series*> keys{&sk};
    const std::vector<const Series*> values{&sv};
    DataFrame r = df::group_agg(keys, values,
                                {AggSpec{AggOp::Distinct, 0, "d", 256.0},
                                 AggSpec{AggOp::Sample, 0, "s", 16.0}},
                                std::vector<std::string>{"k"});
    REQUIRE(r.num_rows() == 1);
    const std::int64_t est = r.column("d").data<std::int64_t>()[0];
    CHECK(est > n / 2);
    CHECK(est < n * 2);
    const Series sample = r.column("s");
    CHECK(sample.offsets()[1] - sample.offsets()[0] == 16);
}

TEST_CASE("a null key is its own group and stays null in the result") {
    // k = [1, null, 1, 0]: the null key must not fold into the 0 group.
    const std::int64_t kv[4] = {1, 0, 1, 0};
    const std::uint8_t kvalid[1] = {0x0d};
    const double yv[4] = {1.0, 3.0, 5.0, 7.0};
    Series k = Series::flat_i64(kv, 4, kvalid);
    Series y = Series::flat_f64(yv, 4);
    std::vector<const Series*> keys{&k};
    std::vector<const Series*> values{&y};
    std::vector<AggSpec> specs{AggSpec{AggOp::Sum, 0, "sum"}};

    auto st = df::agg_new(specs);
    df::agg_accumulate(*st, keys, values);
    DataFrame r = df::agg_finalize(*st, "k");
    REQUIRE(r.num_rows() == 3);
    const Series rk = r.column("k");
    const Series rs = r.column("sum");
    std::int64_t null_row = -1;
    for (std::int64_t i = 0; i < 3; ++i)
        if (rk.is_null(i)) null_row = i;
    REQUIRE(null_row >= 0);
    CHECK(rs.data<double>()[null_row] == doctest::Approx(3.0));
    auto present_row = [&](std::int64_t key) {
        for (std::int64_t i = 0; i < 3; ++i)
            if (!rk.is_null(i) && rk.data<std::int64_t>()[i] == key) return i;
        return std::int64_t{-1};
    };
    CHECK(rs.data<double>()[present_row(1)] == doctest::Approx(6.0));
    CHECK(rs.data<double>()[present_row(0)] == doctest::Approx(7.0));

    // The null flag survives serialize, merge and the seed-side split.
    auto back = df::agg_deserialize(df::agg_serialize(*st));
    check_same(r, df::agg_finalize(*back, "k"));
    auto lo = df::agg_new(specs);
    df::agg_accumulate(*lo, keys, values, 0, 2);
    auto hi = df::agg_new(specs);
    df::agg_accumulate(*hi, keys, values, 2, 4);
    df::agg_merge(*lo, *hi);
    check_same(r, df::agg_finalize(*lo, "k"));
}

TEST_CASE("group_agg_expr reads a value column in a view encoding") {
    // A filter's result is a gather view (Dictionary encoding); the
    // accumulator reads flat columns, so the value must be materialized.
    const double xv[4] = {1.0, 0.0, 3.0, 0.0};
    const std::uint8_t xvalid[1] = {0x05};
    Series x = Series::flat_f64(xv, 4, xvalid);
    const std::uint8_t mask_bits[1] = {0x03};
    Series mask = Series::flat(TypeId::Bool, mask_bits, 4);
    Series xs = x.filter(mask);
    REQUIRE(xs.valid());
    REQUIRE(xs.length() == 2);
    const double yv[2] = {1.0, 3.0};
    Series y = Series::flat_f64(yv, 2);
    const std::int64_t kv[2] = {7, 7};
    Series k = Series::flat_i64(kv, 2);
    std::vector<const Series*> inputs{&k, &xs, &y};
    DataFrame r = df::group_agg_expr(
        df::col(0),
        {df::agg_sum(df::col(1), "sx"), df::agg_sum(df::col(2), "sy")}, inputs,
        "k");
    REQUIRE(r.num_rows() == 1);
    CHECK(r.column("sx").data<double>()[0] == doctest::Approx(1.0));
    CHECK(r.column("sy").data<double>()[0] == doctest::Approx(4.0));
}

TEST_CASE("agg_deserialize survives mutated and truncated blobs") {
    // A deterministic mutation pass over a real state's bytes: every
    // mutation is either decoded or refused with an exception, never read
    // past the blob or crashed on.
    Sample s;
    auto st = df::agg_new(all_specs());
    df::agg_accumulate(*st, s.keys, s.values);
    const std::string blob = df::agg_serialize(*st);
    REQUIRE(blob.size() > 64);
    std::uint64_t x = 0x9e3779b97f4a7c15ULL;
    auto next = [&]() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    std::int64_t decoded = 0, refused = 0;
    for (int round = 0; round < 2000; ++round) {
        std::string m = blob;
        const int edits = 1 + static_cast<int>(next() % 8);
        for (int e = 0; e < edits; ++e)
            m[next() % m.size()] = static_cast<char>(next() & 0xff);
        if (round % 5 == 0) m.resize(next() % m.size());
        try {
            auto back = df::agg_deserialize(m);
            if (back) {
                (void)df::agg_finalize(*back, "k");
                ++decoded;
            } else {
                ++refused;
            }
        } catch (const std::exception&) {
            ++refused;
        }
    }
    CHECK(decoded + refused == 2000);
    CHECK(refused > 0);
}
