#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace df = dftracer::utils::dataframe;
using df::AggDynInput;
using df::AggDynSpec;
using df::AggOp;
using df::AggSpec;
using df::DataFrame;
using df::Series;
using df::TypeId;

namespace {

// Two-key sample: k1 in {0,1}, k2 in {0,1,2}, plus a numeric value, a tag
// string (SetUnion), a name string + a numeric by-column (ArgMax).
struct Sample {
    std::vector<std::int64_t> k1, k2, v, by;
    std::vector<std::string> tag, name;
    Series sk1, sk2, sv, sby, stag, sname;
    std::vector<const Series*> keys2, keys1, values;

    explicit Sample(std::int64_t n) {
        for (std::int64_t i = 0; i < n; ++i) {
            k1.push_back(i % 2);
            k2.push_back(i % 3);
            v.push_back(i);
            by.push_back(i);  // unique so ArgMax has no tie ambiguity
            tag.push_back("t" + std::to_string(i % 5));
            name.push_back("n" + std::to_string(i));
        }
        sk1 = Series::flat_i64(k1.data(), n);
        sk2 = Series::flat_i64(k2.data(), n);
        sv = Series::flat_i64(v.data(), n);
        sby = Series::flat_i64(by.data(), n);
        stag = Series::strings(tag);
        sname = Series::strings(name);
        keys2 = {&sk1, &sk2};
        keys1 = {&sk1};
        values = {&sv, &stag, &sname, &sby};
    }
};

std::vector<AggSpec> mixed_specs() {
    return {
        AggSpec{AggOp::Sum, 0, "sum"},
        AggSpec{AggOp::Count, -1, "cnt"},
        AggSpec{AggOp::Mean, 0, "mean"},
        AggSpec{AggOp::Pct, 0, "p50", 0.5},
        AggSpec{AggOp::Hist, 0, "hist"},
        AggSpec{AggOp::SetUnion, 1, "tags"},
        AggSpec{AggOp::ArgMax, 2, "argmax", 0.0, 3},
    };
}

}  // namespace

TEST_CASE("group_agg_state + agg_finalize matches group_agg") {
    Sample s(4096);
    auto st = df::group_agg_state(s.keys1, s.values, mixed_specs());
    DataFrame a = df::agg_finalize(*st, "k1");
    DataFrame b = df::group_agg(s.keys1, s.values, mixed_specs(),
                                std::vector<std::string>{"k1"});
    REQUIRE(a.num_rows() == b.num_rows());
    REQUIRE(a.names == b.names);
    for (std::int64_t r = 0; r < a.num_rows(); ++r) {
        CHECK(a.column("sum").data<std::int64_t>()[r] ==
              b.column("sum").data<std::int64_t>()[r]);
        CHECK(a.column("cnt").data<std::int64_t>()[r] ==
              b.column("cnt").data<std::int64_t>()[r]);
        CHECK(a.column("mean").data<double>()[r] ==
              doctest::Approx(b.column("mean").data<double>()[r]));
        CHECK(a.column("p50").data<double>()[r] ==
              doctest::Approx(b.column("p50").data<double>()[r]));
        CHECK(a.column("hist").length() == b.column("hist").length());
        CHECK(std::string(a.column("tags").string_at(r)) ==
              std::string(b.column("tags").string_at(r)));
        CHECK(std::string(a.column("argmax").string_at(r)) ==
              std::string(b.column("argmax").string_at(r)));
    }
}

TEST_CASE("group_agg keeps a Uint64 key's type and its > 2^63 value") {
    const std::uint64_t big = (std::uint64_t{1} << 63) + 7;  // negative as i64
    std::vector<std::uint64_t> k{big, big, 3, 3};
    std::vector<std::int64_t> v{1, 2, 3, 4};
    Series sk = Series::flat(TypeId::Uint64, k.data(), 4);
    Series sv = Series::flat_i64(v.data(), 4);
    std::vector<const Series*> keys{&sk};
    std::vector<const Series*> values{&sv};
    DataFrame r = df::group_agg(keys, values, {AggSpec{AggOp::Sum, 0, "sum"}},
                                std::vector<std::string>{"k"});
    REQUIRE(r.column("k").type() == TypeId::Uint64);
    REQUIRE(r.num_rows() == 2);
    const std::uint64_t* kc = r.column("k").data<std::uint64_t>();
    bool saw_big = false, saw_small = false;
    for (std::int64_t i = 0; i < r.num_rows(); ++i) {
        if (kc[i] == big) saw_big = true;
        if (kc[i] == 3) saw_small = true;
    }
    CHECK(saw_big);
    CHECK(saw_small);
}

TEST_CASE("group_agg keeps a Float64 key's type and value") {
    std::vector<double> k{1.5, 1.5, 2.5, 2.5};
    std::vector<std::int64_t> v{1, 2, 3, 4};
    Series sk = Series::flat_f64(k.data(), 4);
    Series sv = Series::flat_i64(v.data(), 4);
    std::vector<const Series*> keys{&sk};
    std::vector<const Series*> values{&sv};
    DataFrame r =
        df::group_agg(keys, values, {AggSpec{AggOp::Count, -1, "cnt"}},
                      std::vector<std::string>{"k"});
    REQUIRE(r.column("k").type() == TypeId::Float64);
    REQUIRE(r.num_rows() == 2);
    const double* kc = r.column("k").data<double>();
    bool saw15 = false, saw25 = false;
    for (std::int64_t i = 0; i < r.num_rows(); ++i) {
        if (kc[i] == 1.5) saw15 = true;
        if (kc[i] == 2.5) saw25 = true;
    }
    CHECK(saw15);
    CHECK(saw25);
}

TEST_CASE("agg_regroup coarsens (k1,k2)->k1 matching a direct k1 group_agg") {
    Sample s(6000);
    auto fine = df::group_agg_state(s.keys2, s.values, mixed_specs());

    auto coarse = df::agg_regroup(*fine, {0});  // keep k1 only
    df::agg_sort_groups(*coarse);
    DataFrame got = df::agg_finalize(*coarse, "k1");

    auto direct_st = df::group_agg_state(s.keys1, s.values, mixed_specs());
    df::agg_sort_groups(*direct_st);
    DataFrame want = df::agg_finalize(*direct_st, "k1");

    REQUIRE(got.num_rows() == want.num_rows());
    for (std::int64_t r = 0; r < got.num_rows(); ++r) {
        CHECK(got.column("k1").data<std::int64_t>()[r] ==
              want.column("k1").data<std::int64_t>()[r]);
        CHECK(got.column("sum").data<std::int64_t>()[r] ==
              want.column("sum").data<std::int64_t>()[r]);
        CHECK(got.column("cnt").data<std::int64_t>()[r] ==
              want.column("cnt").data<std::int64_t>()[r]);
        CHECK(got.column("mean").data<double>()[r] ==
              doctest::Approx(want.column("mean").data<double>()[r]));
        CHECK(got.column("p50").data<double>()[r] ==
              doctest::Approx(want.column("p50").data<double>()[r]));
        CHECK(std::string(got.column("tags").string_at(r)) ==
              std::string(want.column("tags").string_at(r)));
        CHECK(std::string(got.column("argmax").string_at(r)) ==
              std::string(want.column("argmax").string_at(r)));
    }
    // Hist survives regroup as a list column with a row per group.
    CHECK(got.column("hist").length() == want.column("hist").length());
}

TEST_CASE("agg_regroup preserves occupancy busy/active exactly") {
    // ts/dur events across two keys; coarsen to one key and compare busy/active
    // against a direct single-key occupancy aggregation.
    const std::int64_t n = 2000;
    std::vector<std::int64_t> k1(n), k2(n), ts(n), dur(n);
    for (std::int64_t i = 0; i < n; ++i) {
        k1[static_cast<std::size_t>(i)] = i % 2;
        k2[static_cast<std::size_t>(i)] = i % 4;
        ts[static_cast<std::size_t>(i)] = i * 3;
        dur[static_cast<std::size_t>(i)] = 5;
    }
    Series sk1 = Series::flat_i64(k1.data(), n);
    Series sk2 = Series::flat_i64(k2.data(), n);
    Series sts = Series::flat_i64(ts.data(), n);
    Series sdur = Series::flat_i64(dur.data(), n);
    std::vector<const Series*> keys2{&sk1, &sk2};
    std::vector<const Series*> keys1{&sk1};
    std::vector<const Series*> values{&sts, &sdur};
    std::vector<AggSpec> specs{AggSpec{AggOp::Busy, 0, "busy", 0.0, 1},
                               AggSpec{AggOp::Active, 0, "active", 0.0, 1}};

    auto fine = df::group_agg_state(keys2, values, specs);
    auto coarse = df::agg_regroup(*fine, {0});
    df::agg_sort_groups(*coarse);
    DataFrame got = df::agg_finalize(*coarse, "k1");

    auto direct = df::group_agg_state(keys1, values, specs);
    df::agg_sort_groups(*direct);
    DataFrame want = df::agg_finalize(*direct, "k1");

    REQUIRE(got.num_rows() == want.num_rows());
    for (std::int64_t r = 0; r < got.num_rows(); ++r) {
        CHECK(got.column("busy").data<double>()[r] ==
              doctest::Approx(want.column("busy").data<double>()[r]));
        CHECK(got.column("active").data<double>()[r] ==
              doctest::Approx(want.column("active").data<double>()[r]));
    }
}

TEST_CASE("agg_regroup with bucket_recut coarsens the time grain") {
    // key 0 is a time bucket at grain 10; key 1 is a group. Recut to grain 20.
    const std::int64_t n = 4000;
    std::vector<std::int64_t> bucket(n), grp(n), v(n);
    for (std::int64_t i = 0; i < n; ++i) {
        bucket[static_cast<std::size_t>(i)] = (i % 8) * 10;  // 0,10,...,70
        grp[static_cast<std::size_t>(i)] = i % 2;
        v[static_cast<std::size_t>(i)] = i;
    }
    Series sb = Series::flat_i64(bucket.data(), n);
    Series sg = Series::flat_i64(grp.data(), n);
    Series sv = Series::flat_i64(v.data(), n);
    std::vector<const Series*> keys{&sb, &sg};
    std::vector<const Series*> values{&sv};
    std::vector<AggSpec> specs{AggSpec{AggOp::Sum, 0, "sum"},
                               AggSpec{AggOp::Count, -1, "cnt"}};

    auto fine = df::group_agg_state(keys, values, specs);
    auto coarse = df::agg_regroup(*fine, {0, 1}, /*bucket_recut=*/20);
    df::agg_sort_groups(*coarse);
    DataFrame got =
        df::agg_finalize(*coarse, std::vector<std::string>{"bucket", "grp"});

    // Direct: floor each bucket to grain 20 up front, then group.
    std::vector<std::int64_t> cb(n);
    for (std::int64_t i = 0; i < n; ++i)
        cb[static_cast<std::size_t>(i)] =
            (bucket[static_cast<std::size_t>(i)] / 20) * 20;
    Series scb = Series::flat_i64(cb.data(), n);
    std::vector<const Series*> ckeys{&scb, &sg};
    auto direct = df::group_agg_state(ckeys, values, specs);
    df::agg_sort_groups(*direct);
    DataFrame want =
        df::agg_finalize(*direct, std::vector<std::string>{"bucket", "grp"});

    REQUIRE(got.num_rows() == want.num_rows());
    for (std::int64_t r = 0; r < got.num_rows(); ++r) {
        CHECK(got.column("bucket").data<std::int64_t>()[r] ==
              want.column("bucket").data<std::int64_t>()[r]);
        CHECK(got.column("grp").data<std::int64_t>()[r] ==
              want.column("grp").data<std::int64_t>()[r]);
        CHECK(got.column("sum").data<std::int64_t>()[r] ==
              want.column("sum").data<std::int64_t>()[r]);
        CHECK(got.column("cnt").data<std::int64_t>()[r] ==
              want.column("cnt").data<std::int64_t>()[r]);
    }
}

namespace {

// Two dyn reductions (sum, mean) plus a present-count over each discovered arg.
std::vector<AggDynSpec> dyn_specs() {
    return {{AggOp::Sum, 0.0, "sum_"},
            {AggOp::Mean, 0.0, "mean_"},
            {AggOp::Count, 0.0, "count_"}};
}

// One key column {0,0,1,1} plus two Float64 dyn args over rows [0,4).
struct DynSample {
    std::vector<std::int64_t> k{0, 0, 1, 1};
    std::vector<double> x{1, 2, 3, 4};
    std::vector<double> y{10, 20, 30, 40};
    Series sk = Series::flat_i64(k.data(), 4);
    Series sx = Series::flat_f64(x.data(), 4);
    Series sy = Series::flat_f64(y.data(), 4);
    std::vector<const Series*> keys{&sk};
};

}  // namespace

TEST_CASE("AggState dyn: per-batch name discovery unions into each group") {
    DynSample s;
    auto st = df::agg_new({AggSpec{AggOp::Count, -1, "n"}}, dyn_specs());
    // Two batches over the same groups discover disjoint arg names (x then y);
    // each group's dyn set is the union, and the finalized columns are sorted.
    agg_accumulate(*st, s.keys, {}, std::vector<AggDynInput>{{"x", &s.sx}});
    agg_accumulate(*st, s.keys, {}, std::vector<AggDynInput>{{"y", &s.sy}});
    df::agg_sort_groups(*st);
    DataFrame r = df::agg_finalize(*st, "k");

    REQUIRE(r.num_rows() == 2);
    const std::vector<std::string> expect{
        "k", "n", "sum_x", "mean_x", "count_x", "sum_y", "mean_y", "count_y"};
    REQUIRE(r.names == expect);
    // Sum/Mean over a Float64 arg stay Float64; the present-count is Int64.
    CHECK(r.column("sum_x").type() == TypeId::Float64);
    CHECK(r.column("count_x").type() == TypeId::Int64);

    // group k=0 has x={1,2}, y={10,20}; k=1 has x={3,4}, y={30,40}.
    CHECK(r.column("sum_x").data<double>()[0] == doctest::Approx(3.0));
    CHECK(r.column("mean_x").data<double>()[0] == doctest::Approx(1.5));
    CHECK(r.column("count_x").data<std::int64_t>()[0] == 2);
    CHECK(r.column("sum_y").data<double>()[1] == doctest::Approx(70.0));
    CHECK(r.column("count_y").data<std::int64_t>()[1] == 2);
}

TEST_CASE("AggState dyn: an Int64 arg keeps a stable exact Int64 sum column") {
    std::vector<std::int64_t> k{0, 0, 1, 1};
    std::vector<std::int64_t> z{5, 7, 11, 13};
    Series sk = Series::flat_i64(k.data(), 4);
    Series sz = Series::flat_i64(z.data(), 4);
    std::vector<const Series*> keys{&sk};
    auto st = df::agg_new({AggSpec{AggOp::Count, -1, "n"}},
                          {{AggOp::Sum, 0.0, "sum_"}});
    agg_accumulate(*st, keys, {}, std::vector<AggDynInput>{{"z", &sz}});
    df::agg_sort_groups(*st);
    DataFrame r = df::agg_finalize(*st, "k");
    CHECK(r.column("sum_z").type() == TypeId::Int64);
    CHECK(r.column("sum_z").data<std::int64_t>()[0] == 12);
    CHECK(r.column("sum_z").data<std::int64_t>()[1] == 24);
}

TEST_CASE("AggState dyn: merge unions disjoint and overlapping names") {
    DynSample s;
    // State A sees x and y; state B sees y and w, in the same groups.
    std::vector<double> w{100, 200, 300, 400};
    Series sw = Series::flat_f64(w.data(), 4);
    auto a = df::agg_new({AggSpec{AggOp::Count, -1, "n"}}, dyn_specs());
    agg_accumulate(*a, s.keys, {},
                   std::vector<AggDynInput>{{"x", &s.sx}, {"y", &s.sy}});
    auto b = df::agg_new({AggSpec{AggOp::Count, -1, "n"}}, dyn_specs());
    agg_accumulate(*b, s.keys, {},
                   std::vector<AggDynInput>{{"y", &s.sy}, {"w", &sw}});
    df::agg_merge(*a, *b);
    df::agg_sort_groups(*a);
    DataFrame r = df::agg_finalize(*a, "k");

    // Union of names: w, x, y (sorted). y was seen in both, so its count is the
    // sum of both contributions.
    CHECK(r.column_index("sum_w") >= 0);
    CHECK(r.column_index("sum_x") >= 0);
    CHECK(r.column("count_y").data<std::int64_t>()[0] == 4);  // 2 from each
    CHECK(r.column("sum_y").data<double>()[0] ==
          doctest::Approx(60.0));                             // 30+30
    // x only in A, so its group-0 count is the single contribution.
    CHECK(r.column("count_x").data<std::int64_t>()[0] == 2);
    // w only in B.
    CHECK(r.column("sum_w").data<double>()[0] == doctest::Approx(300.0));
}

TEST_CASE("AggState dyn: serialize/deserialize round-trips the side-table") {
    DynSample s;
    auto st = df::agg_new({AggSpec{AggOp::Count, -1, "n"}}, dyn_specs());
    agg_accumulate(*st, s.keys, {},
                   std::vector<AggDynInput>{{"x", &s.sx}, {"y", &s.sy}});
    df::agg_sort_groups(*st);
    DataFrame want = df::agg_finalize(*st, "k");

    std::string blob = df::agg_serialize(*st);
    auto back = df::agg_deserialize(blob);
    DataFrame got = df::agg_finalize(*back, "k");

    REQUIRE(got.names == want.names);
    REQUIRE(got.num_rows() == want.num_rows());
    for (std::int64_t r = 0; r < got.num_rows(); ++r) {
        CHECK(got.column("sum_x").data<double>()[r] ==
              doctest::Approx(want.column("sum_x").data<double>()[r]));
        CHECK(got.column("count_y").data<std::int64_t>()[r] ==
              want.column("count_y").data<std::int64_t>()[r]);
    }
}

TEST_CASE("AggState dyn: a Pct reduction round-trips its per-name sketch") {
    std::vector<std::int64_t> k(1000, 0);
    std::vector<double> x(1000);
    for (std::int64_t i = 0; i < 1000; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<double>(i);
    Series sk = Series::flat_i64(k.data(), 1000);
    Series sx = Series::flat_f64(x.data(), 1000);
    std::vector<const Series*> keys{&sk};
    auto st = df::agg_new({AggSpec{AggOp::Count, -1, "n"}},
                          {{AggOp::Pct, 0.9, "p90_"}});
    agg_accumulate(*st, keys, {}, std::vector<AggDynInput>{{"x", &sx}});
    DataFrame direct = df::agg_finalize(*st, "k");
    const double q = direct.column("p90_x").data<double>()[0];
    CHECK(q == doctest::Approx(900.0).epsilon(0.02));

    auto back = df::agg_deserialize(df::agg_serialize(*st));
    DataFrame r = df::agg_finalize(*back, "k");
    CHECK(r.column("p90_x").data<double>()[0] == doctest::Approx(q));
}

TEST_CASE("agg_regroup over serialize/deserialize round-trip is exact") {
    Sample s(3000);
    auto fine = df::group_agg_state(s.keys2, s.values, mixed_specs());
    // Persist each group as a single-group blob, then rebuild one merged state
    // (the rollup read path), and coarsen that.
    auto rebuilt = df::agg_new(mixed_specs());
    const std::int64_t ng = df::agg_num_groups(*fine);
    for (std::int64_t g = 0; g < ng; ++g) {
        std::string blob = df::agg_serialize(*df::agg_extract_group(*fine, g));
        auto one = df::agg_deserialize(blob);
        df::agg_merge(*rebuilt, *one);
    }
    auto coarse = df::agg_regroup(*rebuilt, {0});
    df::agg_sort_groups(*coarse);
    DataFrame got = df::agg_finalize(*coarse, "k1");

    auto direct = df::group_agg_state(s.keys1, s.values, mixed_specs());
    df::agg_sort_groups(*direct);
    DataFrame want = df::agg_finalize(*direct, "k1");

    REQUIRE(got.num_rows() == want.num_rows());
    for (std::int64_t r = 0; r < got.num_rows(); ++r) {
        CHECK(got.column("sum").data<std::int64_t>()[r] ==
              want.column("sum").data<std::int64_t>()[r]);
        CHECK(got.column("p50").data<double>()[r] ==
              doctest::Approx(want.column("p50").data<double>()[r]));
        CHECK(std::string(got.column("tags").string_at(r)) ==
              std::string(want.column("tags").string_at(r)));
    }
}
