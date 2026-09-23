#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/query/query.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using dftracer::utils::dataframe::Agg;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::GroupAgg;
using dftracer::utils::dataframe::Series;

namespace {
Series i64(std::vector<std::int64_t> v) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
}
// An Int64 column with an Arrow validity bitmap; `valid[i]==0` marks a null.
Series i64_nullable(std::vector<std::int64_t> v, std::vector<int> valid) {
    const std::int64_t n = static_cast<std::int64_t>(v.size());
    std::vector<std::uint8_t> bitmap(static_cast<std::size_t>((n + 7) / 8), 0);
    for (std::int64_t i = 0; i < n; ++i)
        if (valid[static_cast<std::size_t>(i)])
            bitmap[static_cast<std::size_t>(i >> 3)] |= (1u << (i & 7));
    return Series::flat_i64(v.data(), n, bitmap.data());
}
}  // namespace

TEST_CASE("Series reductions via methods") {
    Series a = i64({3, 1, 4, 1, 5});
    CHECK(a.count() == 5);
    CHECK(a.sum().f64() == doctest::Approx(14.0));
    CHECK(a.min().f64() == doctest::Approx(1.0));
    CHECK(a.max().f64() == doctest::Approx(5.0));
    CHECK(a.mean() == doctest::Approx(2.8));
    CHECK(a.nunique() == 4);
    CHECK(a.median() == doctest::Approx(3.0));
}

TEST_CASE("Series arithmetic + operators") {
    Series a = i64({1, 2, 3});
    Series b = i64({10, 20, 30});
    Series sum = a + b;
    CHECK(sum.data<std::int64_t>()[0] == 11);
    CHECK(sum.data<std::int64_t>()[2] == 33);
    Series prod = a.mul(b);
    CHECK(prod.data<std::int64_t>()[1] == 40);
}

TEST_CASE("Series comparison + logical produce masks, drive filter") {
    Series a = i64({5, 15, 25, 35});
    Series big = a > 10;                         // mask: 0,1,1,1
    Series small = a.lt(30);                     // mask: 1,1,1,0
    Series both = big & small;                   // mask: 0,1,1,0
    Series kept = a.filter(both).materialize();  // selection -> flat
    REQUIRE(kept.length() == 2);
    CHECK(kept.data<std::int64_t>()[0] == 15);
    CHECK(kept.data<std::int64_t>()[1] == 25);
}

TEST_CASE("DataFrame select / filter / sort_by / column") {
    DataFrame df;
    df.names = {"id", "val"};
    df.columns.push_back(i64({1, 2, 3, 4}));
    df.columns.push_back(i64({40, 10, 30, 20}));
    CHECK(df.num_rows() == 4);
    CHECK(df.column_index("val") == 1);
    CHECK(df.column_index("missing") == -1);

    DataFrame just_val = df.select({"val"});
    CHECK(just_val.num_columns() == 1);

    DataFrame sorted = df.sort_by("val");
    CHECK(sorted.column("val").data<std::int64_t>()[0] == 10);
    CHECK(sorted.column("id").data<std::int64_t>()[0] == 2);

    DataFrame top2 = df.filter(df.column("val").gt(25));
    CHECK(top2.num_rows() == 2);
}

TEST_CASE("DataFrame group_by aggregates") {
    DataFrame df;
    df.names = {"k", "v"};
    df.columns.push_back(i64({1, 2, 1, 2, 1}));
    df.columns.push_back(i64({10, 5, 20, 7, 30}));
    DataFrame g = df.group_by(
        "k", {GroupAgg{Agg::Sum, "v", "total"}, GroupAgg{Agg::Count, "", "n"}});
    CHECK(g.num_rows() == 2);
    // key 1 -> sum 60 count 3 ; key 2 -> sum 12 count 2 (first-seen order)
    const std::int64_t* total = g.column("total").data<std::int64_t>();
    const std::int64_t* n = g.column("n").data<std::int64_t>();
    CHECK(total[0] == 60);
    CHECK(n[0] == 3);
    CHECK(total[1] == 12);
    CHECK(n[1] == 2);
}

TEST_CASE(
    "group_agg_expr over a String value column: count and count_valid "
    "work, a numeric reducer is refused") {
    namespace df = dftracer::utils::dataframe;
    DataFrame b;
    b.names = {"name", "cycles"};
    b.columns.push_back(
        Series::strings(std::vector<std::string>{"cpu", "gpu", "cpu", "gpu"}));
    // "lots", "few", null, "few": row 2 is null.
    const std::int32_t offsets[5] = {0, 4, 7, 7, 10};
    const std::uint8_t valid[1] = {0x0b};
    b.columns.push_back(Series{dftu_series_new_string(DFTU_TYPE_STRING, offsets,
                                                      "lotsfewfew", 4, valid)});
    std::vector<const Series*> inputs = {&b.columns[0], &b.columns[1]};
    const df::Expr cyc = df::expr_col(1);

    df::AggExprSpec cnt = df::agg_count("n");
    cnt.value = cyc;
    df::AggExprSpec present;
    present.op = df::AggOp::CountValid;
    present.value = cyc;
    present.out = "present";
    DataFrame g =
        df::group_agg_expr(df::expr_col(0), {cnt, present}, inputs, "name");
    REQUIRE(g.num_rows() == 2);
    CHECK(g.column("n").data<std::int64_t>()[0] == 2);
    CHECK(g.column("n").data<std::int64_t>()[1] == 2);
    CHECK(g.column("present").data<std::int64_t>()[0] == 1);  // cpu: row 2 null
    CHECK(g.column("present").data<std::int64_t>()[1] == 2);
    CHECK_THROWS_WITH_AS(
        df::group_agg_expr(df::expr_col(0), {df::agg_sum(cyc, "s")}, inputs,
                           "name"),
        "group_by: aggregate 's' reduces a String column numerically; only "
        "count_valid, first and last apply to strings",
        std::invalid_argument);
}

TEST_CASE("SIMD sketch_bucket_keys matches scalar DDSketch::add") {
    namespace df = dftracer::utils::dataframe;
    std::vector<double> vals;
    vals.reserve(1000);
    for (int i = 0; i < 1000; ++i)
        vals.push_back(i % 7 == 0 ? 0.0
                                  : static_cast<double>((i * 13) % 997) + 0.5);

    df::DDSketch a;  // scalar path
    for (double v : vals) a.add(v);

    df::DDSketch b;  // SIMD bulk-key path
    std::vector<std::int32_t> keys(vals.size());
    df::sketch_bucket_keys(vals.data(), static_cast<std::int64_t>(vals.size()),
                           b.log_gamma(), keys.data());
    for (std::size_t i = 0; i < keys.size(); ++i) b.add_key(keys[i], vals[i]);

    CHECK(a.count() == b.count());
    CHECK(a.min() == b.min());
    CHECK(a.max() == b.max());
    for (double q : {0.1, 0.25, 0.5, 0.75, 0.9, 0.99, 1.0}) {
        CHECK(a.quantile(q) == doctest::Approx(b.quantile(q)));
    }
}

TEST_CASE("DDSketch bins hold more than 65535 values") {
    namespace df = dftracer::utils::dataframe;
    df::DDSketch s;
    for (int i = 0; i < 300000; ++i) s.add(100.0);
    CHECK(s.count() == 300000);
    CHECK(s.quantile(0.5) == doctest::Approx(100.0).epsilon(0.02));
    CHECK(s.quantile(0.99) == doctest::Approx(100.0).epsilon(0.02));
    CHECK(std::isfinite(s.quantile(0.999)));
}

TEST_CASE("DataFrame group_by pct (DDSketch quantile) is close to exact") {
    namespace df = dftracer::utils::dataframe;
    // Two keys, each with a known distribution; the sketch p50/p90 should land
    // within the sketch's relative accuracy of the true quantile.
    const std::int64_t per = 4000;
    std::vector<std::int64_t> k, v;
    k.reserve(static_cast<std::size_t>(2 * per));
    v.reserve(static_cast<std::size_t>(2 * per));
    for (std::int64_t i = 0; i < per; ++i) {
        k.push_back(0);
        v.push_back(i + 1);         // key 0: 1..per uniform
        k.push_back(1);
        v.push_back(10 * (i + 1));  // key 1: 10..10*per
    }
    DataFrame b;
    b.names = {"k", "v"};
    b.columns.push_back(Series::flat_i64(k.data(), 2 * per));
    b.columns.push_back(Series::flat_i64(v.data(), 2 * per));
    DataFrame g = b.group_by("k", {GroupAgg{df::Agg::Pct, "v", "p50", 0.5},
                                   GroupAgg{df::Agg::Pct, "v", "p90", 0.9}});
    REQUIRE(g.num_rows() == 2);
    const std::int64_t* key = g.column("k").data<std::int64_t>();
    const double* p50 = g.column("p50").data<double>();
    const double* p90 = g.column("p90").data<double>();
    for (std::int64_t r = 0; r < 2; ++r) {
        const double scale = key[r] == 0 ? 1.0 : 10.0;
        CHECK(p50[r] == doctest::Approx(scale * per * 0.5).epsilon(0.03));
        CHECK(p90[r] == doctest::Approx(scale * per * 0.9).epsilon(0.03));
    }
}

TEST_CASE("group_agg pct partials serialize / merge round-trip") {
    namespace df = dftracer::utils::dataframe;
    std::vector<std::int64_t> k(2000), v(2000);
    for (std::int64_t i = 0; i < 2000; ++i) {
        k[static_cast<std::size_t>(i)] = i % 2;
        v[static_cast<std::size_t>(i)] = i;
    }
    Series key = Series::flat_i64(k.data(), 2000);
    Series val = Series::flat_i64(v.data(), 2000);
    std::vector<const Series*> vals{&val};

    auto st = df::agg_new({df::AggSpec{df::AggOp::Pct, 0, "p90", 0.9}});
    df::agg_accumulate(*st, key, vals, 0, 1000);
    auto st2 = df::agg_new({df::AggSpec{df::AggOp::Pct, 0, "p90", 0.9}});
    df::agg_accumulate(*st2, key, vals, 1000, 2000);

    // Round-trip the second partial through serialize before merging.
    std::string blob = df::agg_serialize(*st2);
    auto st2b = df::agg_deserialize(blob);
    df::agg_merge(*st, *st2b);

    DataFrame direct;
    {
        auto full = df::agg_new({df::AggSpec{df::AggOp::Pct, 0, "p90", 0.9}});
        df::agg_accumulate(*full, key, vals);
        direct = df::agg_finalize(*full, "k");
    }
    DataFrame merged = df::agg_finalize(*st, "k");
    REQUIRE(merged.num_rows() == direct.num_rows());
    for (std::int64_t r = 0; r < merged.num_rows(); ++r)
        CHECK(merged.column("p90").data<double>()[r] ==
              doctest::Approx(direct.column("p90").data<double>()[r]));
}

TEST_CASE("SIMD arg_min/arg_max: earliest tie, matches scalar, null fallback") {
    namespace df = dftracer::utils::dataframe;
    // 200 f64 values; a unique max in the SIMD region and a tied min so the
    // earliest-index tie-break is exercised.
    std::vector<double> dv(200);
    for (int i = 0; i < 200; ++i) dv[static_cast<std::size_t>(i)] = 5.0;
    dv[137] = 99.0;           // unique max
    dv[3] = -1.0;             // first min
    dv[150] = -1.0;           // tied min, later
    Series s = df::Series::flat_f64(dv.data(), 200);
    CHECK(s.arg_max() == 137);
    CHECK(s.arg_min() == 3);  // earliest of the tied minima

    // Int16 (narrow) exercises the same generic two-pass path.
    std::vector<std::int16_t> iv(100);
    for (int i = 0; i < 100; ++i)
        iv[static_cast<std::size_t>(i)] =
            static_cast<std::int16_t>((i * 7) % 50);
    Series si = df::Series::flat(df::TypeId::Int16, iv.data(), 100);
    // Brute-force reference.
    int ref_max = 0, ref_min = 0;
    for (int i = 1; i < 100; ++i) {
        if (iv[static_cast<std::size_t>(i)] >
            iv[static_cast<std::size_t>(ref_max)])
            ref_max = i;
        if (iv[static_cast<std::size_t>(i)] <
            iv[static_cast<std::size_t>(ref_min)])
            ref_min = i;
    }
    CHECK(si.arg_max() == ref_max);
    CHECK(si.arg_min() == ref_min);

    // A null-bearing column takes the scalar fallback (skips nulls) - still ok.
    Series n = i64_nullable({7, 0, 3, 0, 9}, {1, 0, 1, 0, 1});
    CHECK(n.arg_max() == 4);
    CHECK(n.arg_min() == 2);
}

TEST_CASE("SIMD compare covers narrow integer types (Int16/Uint8)") {
    namespace df = dftracer::utils::dataframe;
    // 100 values so the 64-at-a-time SIMD block plus a scalar tail both run.
    std::vector<std::int16_t> sv(100);
    for (int i = 0; i < 100; ++i)
        sv[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(i - 40);
    Series s = df::Series::flat(df::TypeId::Int16, sv.data(), 100);
    Series mask = s.gt(std::int64_t(0));  // > 0
    Series kept = s.filter(mask).materialize();
    REQUIRE(kept.length() == 59);  // 1..59 (values i-40 > 0 for i in 41..99)
    CHECK(kept.data<std::int16_t>()[0] == 1);

    std::vector<std::uint8_t> uv(100);
    for (int i = 0; i < 100; ++i)
        uv[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    Series u = df::Series::flat(df::TypeId::Uint8, uv.data(), 100);
    Series um = u.le(std::int64_t(9));  // <= 9 -> first 10
    CHECK(u.filter(um).materialize().length() == 10);
}

TEST_CASE("SIMD cast fast paths match static_cast semantics") {
    namespace df = dftracer::utils::dataframe;
    // Values chosen to exercise truncation-toward-zero and sign.
    std::vector<double> dv{-3.9, -0.5, 0.0, 1.5, 2.9, 1000.25};
    Series f64 = df::Series::flat_f64(dv.data(), 6);

    // f64 -> i64 (ConvertTo): truncates toward zero like static_cast.
    Series i = f64.cast(df::TypeId::Int64);
    const std::int64_t exp_i[] = {-3, 0, 0, 1, 2, 1000};
    for (int k = 0; k < 6; ++k) CHECK(i.data<std::int64_t>()[k] == exp_i[k]);

    // i64 -> f64 round-trips the integer values exactly.
    Series back = i.cast(df::TypeId::Float64);
    for (int k = 0; k < 6; ++k)
        CHECK(back.data<double>()[k] == doctest::Approx((double)exp_i[k]));

    // f64 -> f32 -> f64 (Demote/Promote).
    Series f32 = f64.cast(df::TypeId::Float32);
    CHECK(f32.type() == df::TypeId::Float32);
    Series wide = f32.cast(df::TypeId::Float64);
    CHECK(wide.data<double>()[5] == doctest::Approx(1000.25));

    // A width-changing integer pair takes the scalar fallback; still correct.
    std::vector<std::int64_t> lv{1, 2, 300, -5};
    Series i64 = df::Series::flat_i64(lv.data(), 4);
    Series i8 = i64.cast(df::TypeId::Int8);
    CHECK(i8.data<std::int8_t>()[1] == 2);
    CHECK(i8.data<std::int8_t>()[2] == static_cast<std::int8_t>(300));
}

TEST_CASE("DataFrame group_by hist emits a list<struct> per group") {
    namespace df = dftracer::utils::dataframe;
    std::vector<std::int64_t> k, v;
    for (std::int64_t i = 0; i < 1000; ++i) {
        k.push_back(i % 2);
        v.push_back((i % 2 == 0) ? 1 + i % 10 : 100 + i % 10);
    }
    DataFrame b;
    b.names = {"k", "v"};
    b.columns.push_back(Series::flat_i64(k.data(), 1000));
    b.columns.push_back(Series::flat_i64(v.data(), 1000));
    DataFrame g = b.group_by("k", {GroupAgg{df::Agg::Hist, "v", "h"}});
    REQUIRE(g.num_rows() == 2);
    const Series& h = g.column("h");
    CHECK(h.type() == df::TypeId::List);
    // The list values are struct{lo,hi,count} bins; assert the child is a
    // 3-field struct with at least one bin.
    const Series child = h.child(0);
    CHECK(child.type() == df::TypeId::Struct);
    CHECK(dftu_series_num_children(child.handle()) == 3);
    CHECK(child.length() > 0);
}

TEST_CASE("DataFrame group_by first / last") {
    DataFrame df;
    df.names = {"k", "v"};
    df.columns.push_back(i64({1, 2, 1, 2, 1}));
    df.columns.push_back(i64({10, 5, 20, 7, 30}));
    DataFrame g = df.group_by(
        "k", {GroupAgg{Agg::First, "v", "f"}, GroupAgg{Agg::Last, "v", "l"}});
    REQUIRE(g.num_rows() == 2);
    const std::int64_t* f = g.column("f").data<std::int64_t>();
    const std::int64_t* l = g.column("l").data<std::int64_t>();
    CHECK(f[0] == 10);  // key 1: first 10, last 30
    CHECK(l[0] == 30);
    CHECK(f[1] == 5);   // key 2: first 5, last 7
    CHECK(l[1] == 7);
}

TEST_CASE("group_agg first/last is exact across the parallel-chunk merge") {
    namespace df = dftracer::utils::dataframe;
    // > AGG_GRAIN (1<<16) rows so the parallel driver splits into chunks; the
    // per-chunk partials merge in an unspecified order, yet first/last must
    // still be the globally first / last value per key.
    const std::int64_t n = (1 << 16) + 5000;
    std::vector<std::int64_t> k(static_cast<std::size_t>(n));
    std::vector<std::int64_t> v(static_cast<std::size_t>(n));
    for (std::int64_t i = 0; i < n; ++i) {
        k[static_cast<std::size_t>(i)] = i % 3;
        v[static_cast<std::size_t>(i)] = i;  // strictly increasing
    }
    DataFrame b;
    b.names = {"k", "v"};
    b.columns.push_back(Series::flat_i64(k.data(), n));
    b.columns.push_back(Series::flat_i64(v.data(), n));
    DataFrame g = b.group_by("k", {GroupAgg{df::Agg::First, "v", "f"},
                                   GroupAgg{df::Agg::Last, "v", "l"}});
    REQUIRE(g.num_rows() == 3);
    const std::int64_t* key = g.column("k").data<std::int64_t>();
    const std::int64_t* f = g.column("f").data<std::int64_t>();
    const std::int64_t* l = g.column("l").data<std::int64_t>();
    for (std::int64_t r = 0; r < 3; ++r) {
        const std::int64_t kk = key[r];
        CHECK(f[r] == kk);  // first v with v%3==kk is kk
        CHECK(l[r] == kk + 3 * ((n - 1 - kk) / 3));  // largest such v < n
    }
}

TEST_CASE("DataFrame group_by sumsq matches FieldStat::sumsq") {
    DataFrame df;
    df.names = {"k", "v"};
    df.columns.push_back(i64({1, 2, 1, 2, 1}));
    df.columns.push_back(i64({10, 5, 20, 7, 30}));
    namespace df_ns = dftracer::utils::dataframe;
    DataFrame g = df.group_by("k", {GroupAgg{df_ns::Agg::SumSq, "v", "ssq"}});
    REQUIRE(g.num_rows() == 2);
    const std::int64_t* key = g.column("k").data<std::int64_t>();
    const double* ssq = g.column("ssq").data<double>();
    for (std::int64_t r = 0; r < 2; ++r) {
        if (key[r] == 1)
            CHECK(ssq[r] == doctest::Approx(10.0 * 10 + 20.0 * 20 + 30.0 * 30));
        else
            CHECK(ssq[r] == doctest::Approx(5.0 * 5 + 7.0 * 7));
    }
}

TEST_CASE("DataFrame group_by argmax picks the repr at the max by-value") {
    namespace df_ns = dftracer::utils::dataframe;
    DataFrame df;
    df.names = {"k", "name", "dur"};
    df.columns.push_back(i64({1, 1, 1, 2, 2}));
    df.columns.push_back(Series::strings({"a", "b", "c", "x", "y"}));
    df.columns.push_back(i64({10, 30, 20, 5, 8}));
    GroupAgg spec{df_ns::Agg::ArgMax, "name", "argmax_name"};
    spec.by = "dur";
    DataFrame g = df.group_by("k", {spec});
    REQUIRE(g.num_rows() == 2);
    const std::int64_t* key = g.column("k").data<std::int64_t>();
    const Series& out = g.column("argmax_name");
    CHECK(out.type() == df_ns::TypeId::String);
    for (std::int64_t r = 0; r < 2; ++r) {
        if (key[r] == 1)
            CHECK(out.string_at(r) == "b");  // dur=30 is the max for key 1
        else
            CHECK(out.string_at(r) == "y");  // dur=8 is the max for key 2
    }
}

TEST_CASE("DataFrame group_by set_union sorts and joins distinct values") {
    namespace df_ns = dftracer::utils::dataframe;
    DataFrame df;
    df.names = {"k", "tag"};
    df.columns.push_back(i64({1, 1, 1, 2}));
    df.columns.push_back(Series::strings({"posix", "stdio", "posix", "mpi"}));
    DataFrame g =
        df.group_by("k", {GroupAgg{df_ns::Agg::SetUnion, "tag", "tags"}});
    REQUIRE(g.num_rows() == 2);
    const std::int64_t* key = g.column("k").data<std::int64_t>();
    const Series& out = g.column("tags");
    const std::string sep(1, '\x1e');
    for (std::int64_t r = 0; r < 2; ++r) {
        if (key[r] == 1)
            CHECK(out.string_at(r) == "posix" + sep + "stdio");
        else
            CHECK(out.string_at(r) == "mpi");
    }
}

TEST_CASE("group_agg argmax/sumsq/set_union merge and serialize round-trip") {
    namespace df = dftracer::utils::dataframe;
    // Two chunks accumulated into separate partials, one round-tripped through
    // serialize/deserialize, then merged - must equal one direct accumulation.
    std::vector<std::int64_t> k(2000), by(2000);
    std::vector<std::string> names(2000), tags(2000);
    for (std::int64_t i = 0; i < 2000; ++i) {
        const auto si = static_cast<std::size_t>(i);
        k[si] = i % 2;
        by[si] = (i * 37) % 500;
        names[si] = "n" + std::to_string(i % 17);
        tags[si] = "t" + std::to_string(i % 5);
    }
    Series key = Series::flat_i64(k.data(), 2000);
    Series by_col = Series::flat_i64(by.data(), 2000);
    Series name_col = Series::strings(names);
    Series tag_col = Series::strings(tags);
    std::vector<const Series*> vals{&name_col, &by_col, &tag_col};

    df::AggSpec argmax_spec{df::AggOp::ArgMax, 0, "am", 0.0, 1};
    df::AggSpec set_spec{df::AggOp::SetUnion, 2, "su"};
    df::AggSpec sumsq_spec{df::AggOp::SumSq, 1, "ssq"};
    std::vector<df::AggSpec> specs{argmax_spec, set_spec, sumsq_spec};

    auto st1 = df::agg_new(specs);
    df::agg_accumulate(*st1, key, vals, 0, 1000);
    auto st2 = df::agg_new(specs);
    df::agg_accumulate(*st2, key, vals, 1000, 2000);

    std::string blob = df::agg_serialize(*st2);
    auto st2b = df::agg_deserialize(blob);
    df::agg_merge(*st1, *st2b);
    DataFrame merged = df::agg_finalize(*st1, "k");

    auto full = df::agg_new(specs);
    df::agg_accumulate(*full, key, vals);
    DataFrame direct = df::agg_finalize(*full, "k");

    REQUIRE(merged.num_rows() == direct.num_rows());
    for (std::int64_t r = 0; r < merged.num_rows(); ++r) {
        CHECK(merged.column("am").string_at(r) ==
              direct.column("am").string_at(r));
        CHECK(merged.column("su").string_at(r) ==
              direct.column("su").string_at(r));
        CHECK(merged.column("ssq").data<double>()[r] ==
              doctest::Approx(direct.column("ssq").data<double>()[r]));
    }
}

TEST_CASE("group_agg_expr argmax/sumsq/set_union via the expression builders") {
    namespace df = dftracer::utils::dataframe;
    DataFrame b;
    b.names = {"k", "name", "dur", "tag"};
    b.columns.push_back(i64({1, 1, 1, 2, 2}));
    b.columns.push_back(Series::strings({"a", "b", "c", "x", "y"}));
    b.columns.push_back(i64({10, 30, 20, 5, 8}));
    b.columns.push_back(Series::strings({"p", "q", "p", "r", "r"}));
    std::vector<const Series*> inputs;
    for (const Series& c : b.columns) inputs.push_back(&c);

    std::vector<df::AggExprSpec> specs{
        df::agg_argmax(df::expr_col(1), df::expr_col(2), "argmax_name"),
        df::agg_sumsq(df::expr_col(2), "sumsq_dur"),
        df::agg_set_union(df::expr_col(3), "tags"),
    };
    DataFrame g = df::group_agg_expr(df::expr_col(0), specs, inputs, "k");
    REQUIRE(g.num_rows() == 2);
    const std::int64_t* key = g.column("k").data<std::int64_t>();
    for (std::int64_t r = 0; r < 2; ++r) {
        if (key[r] == 1) {
            CHECK(g.column("argmax_name").string_at(r) == "b");
            CHECK(g.column("sumsq_dur").data<double>()[r] ==
                  doctest::Approx(10.0 * 10 + 30.0 * 30 + 20.0 * 20));
            CHECK(g.column("tags").string_at(r) ==
                  "p" + std::string(1, '\x1e') + "q");
        } else {
            CHECK(g.column("argmax_name").string_at(r) == "y");
            CHECK(g.column("sumsq_dur").data<double>()[r] ==
                  doctest::Approx(5.0 * 5 + 8.0 * 8));
            CHECK(g.column("tags").string_at(r) == "r");
        }
    }
}

TEST_CASE(
    "DataFrame::group_by(Expr, AggExprSpec) matches the string overload") {
    namespace df = dftracer::utils::dataframe;
    DataFrame b;
    b.names = {"k", "v"};
    b.columns.push_back(i64({0, 1, 0, 1, 0, 1}));
    b.columns.push_back(i64({1, 2, 3, 4, 5, 6}));

    DataFrame expected = b.group_by(
        "k", {GroupAgg{Agg::Sum, "v", "s"}, GroupAgg{Agg::Mean, "v", "m"}});

    std::vector<df::AggExprSpec> specs{df::agg_sum(df::expr_col(1), "s"),
                                       df::agg_mean(df::expr_col(1), "m")};
    DataFrame got = b.group_by(df::expr_col(0), specs);

    REQUIRE(got.num_rows() == expected.num_rows());
    const std::int64_t* ke = expected.column("k").data<std::int64_t>();
    const std::int64_t* kg = got.column("k").data<std::int64_t>();
    const std::int64_t* se = expected.column("s").data<std::int64_t>();
    const std::int64_t* sg = got.column("s").data<std::int64_t>();
    for (std::int64_t i = 0; i < got.num_rows(); ++i) {
        CHECK(kg[i] == ke[i]);
        CHECK(sg[i] == se[i]);
    }
}

TEST_CASE("dftu_dataframe opaque C ABI: build, inspect, frame ops") {
    Series id = i64({1, 2, 3, 4});
    Series val = i64({40, 10, 30, 20});
    const char* names[] = {"id", "val"};
    dftu_series* cols[] = {id.release(), val.release()};  // ownership moves in
    dftu_dataframe* df = dftu_dataframe_new(names, cols, 2);
    REQUIRE(df != nullptr);
    CHECK(dftu_dataframe_num_rows(df) == 4);
    CHECK(dftu_dataframe_num_columns(df) == 2);
    CHECK(std::string(dftu_dataframe_column_name(df, 1)) == "val");
    CHECK(dftu_dataframe_column_name(df, 5) == nullptr);

    // sort_by "val" ascending -> id column reorders to [2,4,3,1]
    dftu_dataframe* sorted = dftu_dataframe_sort_by(df, "val", 0);
    dftu_series* sid = dftu_dataframe_column(sorted, "id");
    REQUIRE(sid != nullptr);
    CHECK(Series{sid}.data<std::int64_t>()[0] == 2);

    const char* keep[] = {"val"};
    dftu_dataframe* proj = dftu_dataframe_select(df, keep, 1);
    CHECK(dftu_dataframe_num_columns(proj) == 1);
    CHECK(dftu_dataframe_column(proj, "id") == nullptr);

    // filter val > 25 -> 2 rows (40, 30)
    Series mask = Series{dftu_dataframe_column(df, "val")}.gt(std::int64_t(25));
    dftu_dataframe* kept = dftu_dataframe_filter(df, mask.handle());
    CHECK(dftu_dataframe_num_rows(kept) == 2);

    dftu_dataframe_free(kept);
    dftu_dataframe_free(proj);
    dftu_dataframe_free(sorted);
    dftu_dataframe_free(df);
}

TEST_CASE("Expr operators build and evaluate a fused expression") {
    namespace df = dftracer::utils::dataframe;
    Series a = i64({1, 2, 3, 4});
    Series b = i64({10, 20, 30, 40});
    // (a + b) evaluated over [a, b]
    df::Expr e = df::col(0) + df::col(1);
    Series r = df::eval(e, {&a, &b});
    REQUIRE(r.length() == 4);
    CHECK(r.data<std::int64_t>()[0] == 11);
    CHECK(r.data<std::int64_t>()[3] == 44);
    // a comparison expression yields a Bool mask
    df::Expr mask = df::col(0) > std::int64_t(2);
    Series m = df::eval(mask, {&a});
    CHECK(((m.data<std::uint8_t>()[0] >> 0) & 1) == 0);  // 1 > 2 = false
    CHECK(((m.data<std::uint8_t>()[0] >> 3) & 1) == 1);  // 4 > 2 = true
}

TEST_CASE("Expr unary math / clip / cast evaluate") {
    namespace df = dftracer::utils::dataframe;
    std::vector<double> xv{-3.2, 4.7, -1.5, 9.9};
    Series x = df::Series::flat_f64(xv.data(), 4);

    Series fl = df::eval(df::expr_unary(df::UnaryOp::Floor, df::col(0)), {&x});
    CHECK(fl.data<double>()[0] == doctest::Approx(-4.0));
    CHECK(fl.data<double>()[3] == doctest::Approx(9.0));

    Series cl =
        df::eval(df::expr_clip(df::col(0), df::detail::expr_scalar_d(-2.0),
                               df::detail::expr_scalar_d(5.0)),
                 {&x});
    CHECK(cl.data<double>()[0] == doctest::Approx(-2.0));
    CHECK(cl.data<double>()[3] == doctest::Approx(5.0));

    Series n = i64({1, 2, 3, 4});
    Series cf = df::eval(df::expr_cast(df::TypeId::Float64, df::col(0)), {&n});
    CHECK(cf.type() == df::TypeId::Float64);
    CHECK(cf.data<double>()[2] == doctest::Approx(3.0));

    auto unary = [&](df::UnaryOp op, Series& in) {
        return df::eval(df::expr_unary(op, df::col(0)), {&in});
    };

    std::vector<double> pv{1.0, 4.0, 9.0, 16.0};
    Series p = df::Series::flat_f64(pv.data(), 4);
    Series sq = unary(df::UnaryOp::Sqrt, p);
    CHECK(sq.data<double>()[1] == doctest::Approx(2.0));
    CHECK(sq.data<double>()[3] == doctest::Approx(4.0));

    Series sg = unary(df::UnaryOp::Sign, x);
    CHECK(sg.data<double>()[0] == doctest::Approx(-1.0));
    CHECK(sg.data<double>()[1] == doctest::Approx(1.0));

    Series ng = unary(df::UnaryOp::Negate, x);
    CHECK(ng.data<double>()[0] == doctest::Approx(3.2));

    Series tr = unary(df::UnaryOp::Trunc, x);
    CHECK(tr.data<double>()[0] == doctest::Approx(-3.0));
    CHECK(tr.data<double>()[3] == doctest::Approx(9.0));

    // is_nan / is_finite yield a Bool mask over float NaN/inf values.
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> nv{1.0, std::nan(""), inf, 4.0};
    Series nan_col = df::Series::flat_f64(nv.data(), 4);
    Series isn = unary(df::UnaryOp::IsNan, nan_col);
    CHECK(isn.type() == df::TypeId::Bool);
    CHECK(((isn.data<std::uint8_t>()[0] >> 0) & 1) == 0);
    CHECK(((isn.data<std::uint8_t>()[0] >> 1) & 1) == 1);

    Series isf = unary(df::UnaryOp::IsFinite, nan_col);
    CHECK(isf.type() == df::TypeId::Bool);
    CHECK(((isf.data<std::uint8_t>()[0] >> 0) & 1) == 1);  // 1.0 finite
    CHECK(((isf.data<std::uint8_t>()[0] >> 2) & 1) == 0);  // inf not finite
}

TEST_CASE("slice carries the validity bitmap (aligned and non-aligned)") {
    // 12 rows; nulls at indices 1, 4, 7, 10.
    Series c = i64_nullable({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
                            {1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1});
    CHECK(c.null_count() == 4);

    // Non-byte-aligned offset (3): the bitmap is re-packed to bit 0. Rows
    // 3..8 map to original 3,4,5,6,7,8 -> nulls at local 1 and 4.
    Series s = c.slice(3, 6);
    REQUIRE(s.length() == 6);
    CHECK(s.null_count() == 2);
    CHECK(s.data<std::int64_t>()[0] == 3);
    CHECK(s.data<std::int64_t>()[5] == 8);

    // fillna over the sliced view closes exactly those two nulls.
    Series f = s.fillna(dftracer::utils::dataframe::detail::expr_scalar_i(-1));
    CHECK(f.null_count() == 0);
    CHECK(f.data<std::int64_t>()[1] == -1);
    CHECK(f.data<std::int64_t>()[4] == -1);
    CHECK(f.data<std::int64_t>()[0] == 3);
}

TEST_CASE("Expr engine is null-aware: fillna and null-preserving ops") {
    namespace df = dftracer::utils::dataframe;
    // A nullable column reaches the expr engine (the chunk slice now carries
    // its validity bitmap), so fillna sees the nulls.
    Series nul = i64_nullable({5, 0, 7, 0}, {1, 0, 1, 0});
    Series filled = df::eval(
        df::expr_fillna(df::col(0), df::detail::expr_scalar_i(-7)), {&nul});
    CHECK(filled.data<std::int64_t>()[0] == 5);
    CHECK(filled.data<std::int64_t>()[1] == -7);
    CHECK(filled.data<std::int64_t>()[2] == 7);
    CHECK(filled.data<std::int64_t>()[3] == -7);
    CHECK(filled.null_count() == 0);

    // A math op over a nullable column preserves the null positions.
    Series abs_nul =
        df::eval(df::expr_unary(df::UnaryOp::Abs, df::col(0)), {&nul});
    CHECK(abs_nul.null_count() == 2);

    // fillna after abs closes the nulls.
    Series both =
        df::eval(df::expr_fillna(df::expr_unary(df::UnaryOp::Abs, df::col(0)),
                                 df::detail::expr_scalar_i(0)),
                 {&nul});
    CHECK(both.null_count() == 0);
    CHECK(both.data<std::int64_t>()[1] == 0);
}

TEST_CASE("DataFrame tail / reverse / with_row_index / sample") {
    DataFrame df;
    df.names = {"id", "val"};
    df.columns.push_back(i64({1, 2, 3, 4, 5}));
    df.columns.push_back(i64({10, 20, 30, 40, 50}));

    DataFrame t = df.tail(2);
    REQUIRE(t.num_rows() == 2);
    CHECK(t.column("id").data<std::int64_t>()[0] == 4);
    CHECK(t.column("id").data<std::int64_t>()[1] == 5);

    DataFrame r = df.reverse();
    REQUIRE(r.num_rows() == 5);
    CHECK(r.column("id").data<std::int64_t>()[0] == 5);
    CHECK(r.column("val").data<std::int64_t>()[0] == 50);

    DataFrame wi = df.with_row_index("row");
    REQUIRE(wi.num_columns() == 3);
    CHECK(wi.names[0] == "row");
    CHECK(wi.column("row").data<std::int64_t>()[0] == 0);
    CHECK(wi.column("row").data<std::int64_t>()[4] == 4);

    // Deterministic: same seed -> same rows, and the sampled rows stay aligned.
    DataFrame s1 = df.sample(3, 42);
    DataFrame s2 = df.sample(3, 42);
    REQUIRE(s1.num_rows() == 3);
    for (std::int64_t i = 0; i < 3; ++i) {
        std::int64_t id = s1.column("id").data<std::int64_t>()[i];
        CHECK(s2.column("id").data<std::int64_t>()[i] == id);
        CHECK(s1.column("val").data<std::int64_t>()[i] == id * 10);
    }
}

TEST_CASE("DataFrame sort_by_multi lexicographic") {
    DataFrame df;
    df.names = {"a", "b"};
    df.columns.push_back(i64({2, 1, 2, 1}));
    df.columns.push_back(i64({9, 8, 7, 6}));
    DataFrame s = df.sort_by_multi({"a", "b"}, false);
    // a asc then b asc: (1,6),(1,8),(2,7),(2,9)
    const std::int64_t* a = s.column("a").data<std::int64_t>();
    const std::int64_t* b = s.column("b").data<std::int64_t>();
    CHECK(a[0] == 1);
    CHECK(b[0] == 6);
    CHECK(a[1] == 1);
    CHECK(b[1] == 8);
    CHECK(a[2] == 2);
    CHECK(b[2] == 7);
    CHECK(a[3] == 2);
    CHECK(b[3] == 9);
}

TEST_CASE("DataFrame sort_by_multi per-column direction") {
    DataFrame df;
    df.names = {"a", "b"};
    df.columns.push_back(i64({2, 1, 2, 1}));
    df.columns.push_back(i64({9, 8, 7, 6}));

    // [false, true]: a asc, b desc -> (1,8),(1,6),(2,9),(2,7).
    DataFrame mixed =
        df.sort_by_multi({"a", "b"}, std::vector<bool>{false, true});
    const std::int64_t* a = mixed.column("a").data<std::int64_t>();
    const std::int64_t* b = mixed.column("b").data<std::int64_t>();
    CHECK(a[0] == 1);
    CHECK(b[0] == 8);
    CHECK(a[1] == 1);
    CHECK(b[1] == 6);
    CHECK(a[2] == 2);
    CHECK(b[2] == 9);
    CHECK(a[3] == 2);
    CHECK(b[3] == 7);

    // Distinct from [true, true] (both descending).
    DataFrame both =
        df.sort_by_multi({"a", "b"}, std::vector<bool>{true, true});
    const std::int64_t* a2 = both.column("a").data<std::int64_t>();
    const std::int64_t* b2 = both.column("b").data<std::int64_t>();
    CHECK(a2[0] == 2);
    CHECK(b2[0] == 9);
    CHECK_FALSE((a2[0] == a[0] && b2[0] == b[0]));

    // A single-flag list broadcasts to every key.
    DataFrame broadcast = df.sort_by_multi({"a", "b"}, std::vector<bool>{true});
    CHECK(broadcast.column("a").data<std::int64_t>()[0] ==
          both.column("a").data<std::int64_t>()[0]);
    CHECK(broadcast.column("b").data<std::int64_t>()[0] ==
          both.column("b").data<std::int64_t>()[0]);

    CHECK_THROWS_AS(df.sort_by_multi({"a", "b"}, std::vector<bool>{}),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        df.sort_by_multi({"a", "b"}, std::vector<bool>{true, true, false}),
        std::invalid_argument);
}

TEST_CASE("DataFrame unique / is_duplicated / is_unique") {
    DataFrame df;
    df.names = {"k", "v"};
    df.columns.push_back(i64({1, 2, 1, 3, 2}));
    df.columns.push_back(i64({7, 8, 7, 9, 8}));
    // rows: (1,7) dup, (2,8) dup, (1,7) dup, (3,9) unique, (2,8) dup
    DataFrame u = df.drop_duplicates();
    REQUIRE(u.num_rows() == 3);  // (1,7),(2,8),(3,9) keep first
    CHECK(u.column("k").data<std::int64_t>()[0] == 1);
    CHECK(u.column("k").data<std::int64_t>()[2] == 3);

    Series dup = df.is_duplicated();
    const std::uint8_t* db = dup.data<std::uint8_t>();
    auto bit = [](const std::uint8_t* p, std::int64_t i) {
        return (p[i >> 3] >> (i & 7)) & 1;
    };
    CHECK(bit(db, 0) == 1);
    CHECK(bit(db, 3) == 0);  // (3,9) not duplicated
    Series uniq = df.is_unique();
    const std::uint8_t* ub = uniq.data<std::uint8_t>();
    CHECK(bit(ub, 3) == 1);
    CHECK(bit(ub, 0) == 0);
}

TEST_CASE("DataFrame drop_nulls / fill_null / null_count") {
    DataFrame df;
    df.names = {"a", "b"};
    df.columns.push_back(i64_nullable({1, 2, 3, 4}, {1, 0, 1, 1}));
    df.columns.push_back(i64_nullable({5, 6, 7, 8}, {1, 1, 1, 0}));

    DataFrame nc = df.null_count();
    REQUIRE(nc.num_rows() == 1);
    CHECK(nc.column("a").data<std::int64_t>()[0] == 1);
    CHECK(nc.column("b").data<std::int64_t>()[0] == 1);

    DataFrame dn = df.drop_nulls();
    REQUIRE(dn.num_rows() == 2);  // rows 0 and 2 fully non-null
    CHECK(dn.column("a").data<std::int64_t>()[0] == 1);
    CHECK(dn.column("a").data<std::int64_t>()[1] == 3);

    DataFrame f = df.fill_null(-1);
    CHECK(f.column("a").null_count() == 0);
    CHECK(f.column("a").data<std::int64_t>()[1] == -1);
    CHECK(f.column("b").data<std::int64_t>()[3] == -1);
}

TEST_CASE("Ergonomic scalar API: no raw dftu_scalar at the call site") {
    // A bare literal 0 must resolve unambiguously against the dftu_scalar
    // overload.
    DataFrame df;
    df.names = {"a"};
    df.columns.push_back(i64_nullable({1, 2, 3, 4}, {1, 0, 1, 1}));
    DataFrame f = df.fill_null(0);
    CHECK(f.column("a").null_count() == 0);
    CHECK(f.column("a").data<std::int64_t>()[1] == 0);

    Series ints = i64({-5, 5, 50, 500});
    Series bounded = ints.clip(0, 100);
    CHECK(bounded.data<std::int64_t>()[0] == 0);
    CHECK(bounded.data<std::int64_t>()[1] == 5);
    CHECK(bounded.data<std::int64_t>()[3] == 100);

    std::vector<double> raw = {1.0, 2.0, 3.0};
    Series floats = Series::flat_f64(raw.data(), 3);
    Series filled = floats.fillna(1.5);
    CHECK(filled.length() == 3);

    // Factory helpers cover anywhere a bare dftu_scalar is still needed.
    namespace df_ns = dftracer::utils::dataframe;
    Series still = ints.clip(df_ns::i64(0), df_ns::i64(100));
    CHECK(still.data<std::int64_t>()[0] == 0);
    Series fnan = floats.fillna(df_ns::f64(2.5));
    CHECK(fnan.length() == 3);
}

TEST_CASE("DataFrame describe") {
    DataFrame df;
    df.names = {"x"};
    df.columns.push_back(i64({2, 4, 6, 8}));
    DataFrame d = df.describe();
    REQUIRE(d.num_rows() == 6);   // count,null_count,mean,std,min,max
    CHECK(d.num_columns() == 2);  // statistic + x
    CHECK(d.names[0] == "statistic");
    CHECK(d.names[1] == "x");
    const double* x = d.column("x").data<double>();
    CHECK(x[0] == doctest::Approx(4.0));  // count
    CHECK(x[1] == doctest::Approx(0.0));  // null_count
    CHECK(x[2] == doctest::Approx(5.0));  // mean
    CHECK(x[4] == doctest::Approx(2.0));  // min
    CHECK(x[5] == doctest::Approx(8.0));  // max
}

TEST_CASE("Series value_counts most-frequent first") {
    Series v = i64({1, 2, 1, 3, 1, 2});
    dftu_dataframe* vc = dftu_series_value_counts(v.handle());
    REQUIRE(vc != nullptr);
    CHECK(dftu_dataframe_num_rows(vc) == 3);
    dftu_series* value = dftu_dataframe_column(vc, "value");
    dftu_series* count = dftu_dataframe_column(vc, "count");
    REQUIRE(value != nullptr);
    REQUIRE(count != nullptr);
    // most frequent is 1 (x3)
    CHECK(Series{value}.data<std::int64_t>()[0] == 1);
    CHECK(Series{count}.data<std::int64_t>()[0] == 3);
    dftu_dataframe_free(vc);
}

TEST_CASE("dftu_dataframe new frame ops via C ABI") {
    Series id = i64({1, 2, 3, 3});
    Series val = i64({10, 20, 30, 30});
    const char* names[] = {"id", "val"};
    dftu_series* cols[] = {id.release(), val.release()};
    dftu_dataframe* df = dftu_dataframe_new(names, cols, 2);
    REQUIRE(df != nullptr);

    dftu_dataframe* uniq = dftu_dataframe_unique(df);
    CHECK(dftu_dataframe_num_rows(uniq) == 3);  // (3,30) deduped

    dftu_dataframe* rev = dftu_dataframe_reverse(df);
    dftu_series* rid = dftu_dataframe_column(rev, "id");
    CHECK(Series{rid}.data<std::int64_t>()[0] == 3);

    dftu_dataframe* tl = dftu_dataframe_tail(df, 1);
    CHECK(dftu_dataframe_num_rows(tl) == 1);

    dftu_series* dupmask = dftu_dataframe_is_duplicated(df);
    REQUIRE(dupmask != nullptr);
    dftu_series_free(dupmask);

    dftu_dataframe_free(tl);
    dftu_dataframe_free(rev);
    dftu_dataframe_free(uniq);
    dftu_dataframe_free(df);
}

TEST_CASE("DataFrame unpivot / melt wide -> long") {
    DataFrame df;
    df.names = {"id", "a", "b"};
    df.columns.push_back(i64({1, 2}));
    df.columns.push_back(i64({10, 20}));
    df.columns.push_back(i64({30, 40}));

    DataFrame m = df.unpivot({"id"}, {"a", "b"});
    REQUIRE(m.num_rows() == 4);  // 2 rows * 2 value_vars
    REQUIRE(m.num_columns() == 3);
    CHECK(m.names[0] == "id");
    CHECK(m.names[1] == "variable");
    CHECK(m.names[2] == "value");
    // Block order: all of "a" then all of "b".
    const std::int64_t* id = m.column("id").data<std::int64_t>();
    const std::int64_t* val = m.column("value").data<std::int64_t>();
    CHECK(id[0] == 1);
    CHECK(id[1] == 2);
    CHECK(id[2] == 1);
    CHECK(id[3] == 2);
    CHECK(val[0] == 10);
    CHECK(val[1] == 20);
    CHECK(val[2] == 30);
    CHECK(val[3] == 40);
    Series var = m.column("variable");
    CHECK(var.string_at(0) == "a");
    CHECK(var.string_at(3) == "b");

    // melt is an alias.
    DataFrame m2 = df.melt({"id"}, {"a", "b"});
    CHECK(m2.num_rows() == 4);
}

TEST_CASE("DataFrame explode a List column") {
    DataFrame df;
    df.names = {"id", "vals"};
    df.columns.push_back(i64({1, 2, 3}));
    // Lists: [10,20], [], [30]  (row 1 empty -> one null row)
    std::vector<std::int32_t> offsets = {0, 2, 2, 3};
    Series child = i64({10, 20, 30});
    df.columns.push_back(Series::list(offsets, std::move(child)));

    DataFrame e = df.explode("vals");
    REQUIRE(e.num_rows() == 4);  // 2 + 1(null) + 1
    const std::int64_t* id = e.column("id").data<std::int64_t>();
    CHECK(id[0] == 1);
    CHECK(id[1] == 1);
    CHECK(id[2] == 2);  // empty-list row repeated once
    CHECK(id[3] == 3);
    Series v = e.column("vals");
    CHECK(v.data<std::int64_t>()[0] == 10);
    CHECK(v.data<std::int64_t>()[1] == 20);
    CHECK(v.is_null(2));  // empty list -> null value
    CHECK(v.data<std::int64_t>()[3] == 30);
}

TEST_CASE("DataFrame to_dummies one-hot") {
    DataFrame df;
    df.names = {"id", "k"};
    df.columns.push_back(i64({1, 2, 3, 4}));
    df.columns.push_back(i64({7, 8, 7, 9}));  // distinct sorted: 7,8,9

    DataFrame oh = df.to_dummies("k");
    // id + three dummy columns (7,8,9)
    REQUIRE(oh.num_columns() == 4);
    CHECK(oh.names[0] == "id");
    CHECK(oh.names[1] == "k_7");
    CHECK(oh.names[2] == "k_8");
    CHECK(oh.names[3] == "k_9");
    const std::int8_t* d7 = oh.column("k_7").data<std::int8_t>();
    const std::int8_t* d8 = oh.column("k_8").data<std::int8_t>();
    const std::int8_t* d9 = oh.column("k_9").data<std::int8_t>();
    CHECK(d7[0] == 1);
    CHECK(d7[2] == 1);
    CHECK(d7[1] == 0);
    CHECK(d8[1] == 1);
    CHECK(d9[3] == 1);
    CHECK(d9[0] == 0);
}

TEST_CASE("DataFrame pivot long->wide") {
    using dftracer::utils::dataframe::TypeId;
    DataFrame df;
    df.names = {"idx", "col", "val"};
    df.columns.push_back(i64({1, 1, 2, 1}));
    df.columns.push_back(Series::strings({"a", "b", "a", "a"}));
    df.columns.push_back(i64({10, 20, 30, 40}));
    // (idx=1, col=a) collides across rows 0 (10) and 3 (40).

    SUBCASE("first keeps the earliest, null where a pair is absent") {
        DataFrame p = df.pivot("idx", "col", "val", "first");
        REQUIRE(p.num_rows() == 2);     // idx 1, 2 (sorted)
        REQUIRE(p.num_columns() == 3);  // idx + col a, b (sorted)
        CHECK(p.names[0] == "idx");
        CHECK(p.names[1] == "a");
        CHECK(p.names[2] == "b");
        Series a = p.column("a");
        Series b = p.column("b");
        CHECK(a.data<std::int64_t>()[0] == 10);  // idx1: first of (10,40)
        CHECK(a.data<std::int64_t>()[1] == 30);  // idx2
        CHECK(b.data<std::int64_t>()[0] == 20);  // idx1 col b
        CHECK(b.is_null(1));                     // idx2 has no col b
    }
    SUBCASE("typed Agg overload matches the string form") {
        DataFrame p = df.pivot("idx", "col", "val", Agg::First);
        REQUIRE(p.num_rows() == 2);
        CHECK(p.column("a").data<std::int64_t>()[0] == 10);
        CHECK(p.column("a").data<std::int64_t>()[1] == 30);
    }
    SUBCASE("mean reduces collisions to Float64") {
        DataFrame p = df.pivot("idx", "col", "val", "mean");
        Series a = p.column("a");
        CHECK(a.type() == TypeId::Float64);
        CHECK(a.data<double>()[0] == doctest::Approx(25.0));  // (10+40)/2
        CHECK(a.data<double>()[1] == doctest::Approx(30.0));
    }
}

TEST_CASE("DataFrame group_by_dynamic tumbling and sliding") {
    DataFrame df;
    df.names = {"t", "v"};
    df.columns.push_back(i64({0, 1, 2, 3, 4, 5}));
    df.columns.push_back(i64({10, 20, 30, 40, 50, 60}));

    SUBCASE("tumbling (period defaults to every)") {
        DataFrame g = df.group_by_dynamic(
            "t", 2, 0,
            {GroupAgg{Agg::Sum, "v", "sv"}, GroupAgg{Agg::Count, "", "n"}});
        REQUIRE(g.num_rows() == 3);
        const std::int64_t* start = g.column("t").data<std::int64_t>();
        const std::int64_t* sv = g.column("sv").data<std::int64_t>();
        const std::int64_t* n = g.column("n").data<std::int64_t>();
        CHECK(start[0] == 0);
        CHECK(sv[0] == 30);   // [0,2): 10+20
        CHECK(n[0] == 2);
        CHECK(start[1] == 2);
        CHECK(sv[1] == 70);   // [2,4): 30+40
        CHECK(start[2] == 4);
        CHECK(sv[2] == 110);  // [4,6): 50+60
    }
    SUBCASE("sliding (period > every overlaps)") {
        DataFrame g =
            df.group_by_dynamic("t", 2, 4, {GroupAgg{Agg::Sum, "v", "sv"}});
        REQUIRE(g.num_rows() == 3);
        const std::int64_t* start = g.column("t").data<std::int64_t>();
        const std::int64_t* sv = g.column("sv").data<std::int64_t>();
        CHECK(start[0] == 0);
        CHECK(sv[0] == 100);  // [0,4): 10+20+30+40
        CHECK(start[1] == 2);
        CHECK(sv[1] == 180);  // [2,6): 30+40+50+60
        CHECK(start[2] == 4);
        CHECK(sv[2] == 110);  // [4,8): 50+60
    }
}

TEST_CASE("dftu_dataframe reshaping ops via C ABI") {
    Series id = i64({1, 2});
    Series a = i64({10, 20});
    Series bcol = i64({30, 40});
    const char* names[] = {"id", "a", "b"};
    dftu_series* cols[] = {id.release(), a.release(), bcol.release()};
    dftu_dataframe* df = dftu_dataframe_new(names, cols, 3);
    REQUIRE(df != nullptr);

    const char* ids[] = {"id"};
    const char* vals[] = {"a", "b"};
    dftu_dataframe* m = dftu_dataframe_unpivot(df, ids, 1, vals, 2);
    REQUIRE(m != nullptr);
    CHECK(dftu_dataframe_num_rows(m) == 4);
    CHECK(dftu_dataframe_num_columns(m) == 3);

    dftu_dataframe* oh = dftu_dataframe_to_dummies(df, "a");
    REQUIRE(oh != nullptr);
    // a has distinct 10,20 -> id, a_10, a_20, b
    CHECK(dftu_dataframe_num_columns(oh) == 4);

    // absent column -> NULL
    CHECK(dftu_dataframe_explode(df, "missing") == nullptr);
    // non-list column -> NULL
    CHECK(dftu_dataframe_explode(df, "a") == nullptr);

    // pivot via the C ABI: index=id, columns=id, values=a, "first".
    dftu_dataframe* pv = dftu_dataframe_pivot(df, "id", "id", "a", "first");
    REQUIRE(pv != nullptr);
    CHECK(dftu_dataframe_num_rows(pv) == 2);  // distinct id 1,2
    CHECK(dftu_dataframe_pivot(df, "missing", "id", "a", "first") == nullptr);

    // group_by_dynamic via the C ABI over the "id" time column (1,2).
    dftu_group_agg gaggs[] = {{"sum", "a", "sa"}, {"count", nullptr, "n"}};
    dftu_dataframe* gd =
        dftu_dataframe_group_by_dynamic(df, "id", 5, 0, gaggs, 2);
    REQUIRE(gd != nullptr);
    CHECK(dftu_dataframe_num_rows(gd) == 1);  // one window [0,5) holds id 1,2
    CHECK(dftu_dataframe_group_by_dynamic(df, "id", 0, 0, gaggs, 2) == nullptr);

    dftu_dataframe_free(gd);
    dftu_dataframe_free(pv);
    dftu_dataframe_free(oh);
    dftu_dataframe_free(m);
    dftu_dataframe_free(df);
}

TEST_CASE("DataFrame mask from a compiled query") {
    DataFrame df;
    df.names = {"cat", "dur"};
    df.columns.push_back(
        Series::strings({"POSIX", "STDIO", "POSIX", "MPI", "POSIX"}));
    df.columns.push_back(i64({10, 20, 30, 40, 50}));

    auto q = dftracer::utils::query::parse_or_throw("cat == \"POSIX\"");
    Series m = df.mask(q);
    REQUIRE(m.length() == 5);
    const std::uint8_t* bits = m.data<std::uint8_t>();
    auto set = [&](std::int64_t i) { return (bits[i >> 3] >> (i & 7)) & 1; };
    CHECK(set(0));
    CHECK_FALSE(set(1));
    CHECK(set(2));
    CHECK_FALSE(set(3));
    CHECK(set(4));

    DataFrame kept = df.filter(m);
    REQUIRE(kept.num_rows() == 3);
    const std::int64_t* dur = kept.column("dur").data<std::int64_t>();
    CHECK(dur[0] == 10);
    CHECK(dur[1] == 30);
    CHECK(dur[2] == 50);
}

TEST_CASE("DataFrame mask throws when a predicate cannot be lowered") {
    DataFrame df;
    df.names = {"cat"};
    df.columns.push_back(Series::strings({"POSIX", "STDIO"}));
    // References a column absent from the frame; no columnar lowering exists.
    auto q = dftracer::utils::query::parse_or_throw("dur > 5");
    CHECK_THROWS_AS((void)df.mask(q), std::runtime_error);
}

TEST_CASE("Bool column take / filter / reverse gather bits, not bytes") {
    using dftracer::utils::dataframe::TypeId;
    // 12 rows so the gather crosses a byte boundary: bit i set iff i % 3 == 0.
    std::vector<std::uint8_t> bits{0b01001001, 0b00000010};
    Series b = Series::flat(TypeId::Bool, bits.data(), 12);
    auto bit_at = [](const Series& s, std::int64_t i) {
        return ((s.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) != 0;
    };

    Series t = b.take({9, 1, 6, 11});
    REQUIRE(t.valid());
    REQUIRE(t.type() == TypeId::Bool);
    REQUIRE(t.length() == 4);
    CHECK(bit_at(t, 0) == true);
    CHECK(bit_at(t, 1) == false);
    CHECK(bit_at(t, 2) == true);
    CHECK(bit_at(t, 3) == false);

    Series r = b.reverse().materialize();
    REQUIRE(r.length() == 12);
    for (std::int64_t i = 0; i < 12; ++i)
        CHECK(bit_at(r, i) == ((11 - i) % 3 == 0));

    Series n = b.take({-1, 3, -1});
    REQUIRE(n.valid());
    CHECK(n.is_null(0));
    CHECK_FALSE(n.is_null(1));
    CHECK(bit_at(n, 1) == true);
    CHECK(n.is_null(2));
}

TEST_CASE("null_mask and valid_mask read the validity bitmap") {
    using dftracer::utils::dataframe::TypeId;
    Series s = i64_nullable({1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
                            {1, 0, 1, 1, 0, 1, 1, 1, 0, 1});
    auto bit_at = [](const Series& m, std::int64_t i) {
        return ((m.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) != 0;
    };
    Series nulls = s.null_mask();
    Series valid = s.valid_mask();
    REQUIRE(nulls.valid());
    REQUIRE(valid.valid());
    CHECK(nulls.type() == TypeId::Bool);
    CHECK(nulls.length() == 10);
    CHECK(nulls.null_count() == 0);
    for (std::int64_t i = 0; i < 10; ++i) {
        CHECK(bit_at(nulls, i) == s.is_null(i));
        CHECK(bit_at(valid, i) == !s.is_null(i));
    }
    Series dense = i64({1, 2, 3});
    Series no_nulls = dense.null_mask();
    Series all_valid = dense.valid_mask();
    for (std::int64_t i = 0; i < 3; ++i) {
        CHECK_FALSE(bit_at(no_nulls, i));
        CHECK(bit_at(all_valid, i));
    }
}

TEST_CASE("column-column arithmetic keeps a null where either side is null") {
    Series a = i64_nullable({1, 2, 3, 4}, {1, 0, 1, 1});
    Series b = i64_nullable({10, 20, 30, 40}, {1, 1, 0, 1});
    for (const Series& r : {a.add(b), a.sub(b), a.mul(b), a.div(b)}) {
        REQUIRE(r.valid());
        CHECK(r.null_count() == 2);
        CHECK_FALSE(r.is_null(0));
        CHECK(r.is_null(1));
        CHECK(r.is_null(2));
        CHECK_FALSE(r.is_null(3));
    }
    CHECK(a.add(b).data<std::int64_t>()[0] == 11);
    CHECK(a.sub(b).data<std::int64_t>()[3] == -36);
    Series dense = i64({1, 2, 3, 4});
    CHECK(dense.add(dense).null_count() == 0);
    CHECK(dense.add(a).null_count() == 1);
}

// Concatenating parts whose seams fall inside a byte: the validity and a
// Bool column's bits around every seam must land exactly, part by part,
// with no part clobbering its neighbour's bits in the shared byte.
TEST_CASE("concat keeps the bits at unaligned seams") {
    std::vector<std::int64_t> v;
    std::vector<int> ok;
    std::vector<const DataFrame*> ptrs;
    std::vector<DataFrame> parts;
    // Part lengths 3, 5, 11, 2, 7: every seam is unaligned.
    const int lens[5] = {3, 5, 11, 2, 7};
    std::int64_t next = 0;
    for (int len : lens) {
        std::vector<std::int64_t> pv;
        std::vector<int> pok;
        for (int i = 0; i < len; ++i) {
            pv.push_back(next);
            pok.push_back(next % 3 != 0);  // nulls at every third row
            v.push_back(next);
            ok.push_back(next % 3 != 0);
            ++next;
        }
        DataFrame d;
        d.names = {"x"};
        d.columns.push_back(i64_nullable(pv, pok));
        parts.push_back(std::move(d));
    }
    for (const DataFrame& d : parts) ptrs.push_back(&d);
    DataFrame all = dftracer::utils::dataframe::concat(
        ptrs, dftracer::utils::dataframe::ConcatHow::Vertical);
    REQUIRE(all.num_rows() == next);
    const Series& x = all.columns[0];
    std::int64_t nulls = 0;
    for (std::int64_t i = 0; i < next; ++i) {
        CHECK(x.is_null(i) == (ok[static_cast<std::size_t>(i)] == 0));
        if (x.is_null(i)) ++nulls;
    }
    CHECK(x.null_count() == nulls);
    // The same seams through a Bool column: x > 10 per part, then concat.
    std::vector<DataFrame> bparts;
    std::vector<const DataFrame*> bptrs;
    for (const DataFrame& d : parts) {
        DataFrame b;
        b.names = {"m"};
        b.columns.push_back(d.columns[0] > std::int64_t{10});
        bparts.push_back(std::move(b));
    }
    for (const DataFrame& d : bparts) bptrs.push_back(&d);
    DataFrame ball = dftracer::utils::dataframe::concat(
        bptrs, dftracer::utils::dataframe::ConcatHow::Vertical);
    const std::uint8_t* bits = ball.columns[0].data<std::uint8_t>();
    for (std::int64_t i = 0; i < next; ++i) {
        const bool set = (bits[i >> 3] >> (i & 7)) & 1;
        if (ok[static_cast<std::size_t>(i)]) CHECK(set == (i > 10));
    }
}
