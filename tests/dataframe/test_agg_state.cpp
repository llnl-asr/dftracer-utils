// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/agg/detail.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <task_result.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

// Two-key sample: k1 in {0,1}, k2 in {0,1,2}, plus a numeric value, a tag
// string (SetUnion), a name string + a numeric by-column (ArgMax).
struct Sample {
    dstd::Vector<std::int64_t> k1, k2, v, by;
    dstd::Vector<dstd::String> tag, name;
    Series sk1, sk2, sv, sby, stag, sname;
    dstd::Vector<const Series*> keys2, keys1, values;

    explicit Sample(std::int64_t n) {
        for (std::int64_t i = 0; i < n; ++i) {
            k1.push_back(i % 2);
            k2.push_back(i % 3);
            v.push_back(i);
            by.push_back(i);  // unique so ArgMax has no tie ambiguity
            tag.push_back(dstd::fmt<"t{}">(i % 5));
            name.push_back(dstd::fmt<"n{}">(i));
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

dstd::Vector<AggSpec> mixed_specs() {
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
    auto st = dftu_utils_test::ok(
        dataframe::group_agg_state(s.keys1, s.values, mixed_specs()));
    DataFrame a = dataframe::agg_finalize(*st, "k1");
    DataFrame b = dftu_utils_test::ok(dataframe::group_agg(
        s.keys1, s.values, mixed_specs(), dstd::Vector<dstd::String>{"k1"}));
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
        CHECK(dstd::String(a.column("tags").string_at(r)) ==
              dstd::String(b.column("tags").string_at(r)));
        CHECK(dstd::String(a.column("argmax").string_at(r)) ==
              dstd::String(b.column("argmax").string_at(r)));
    }
}

TEST_CASE("group_agg keeps a Uint64 key's type and its > 2^63 value") {
    const std::uint64_t big = (std::uint64_t{1} << 63) + 7;  // negative as i64
    dstd::Vector<std::uint64_t> k{big, big, 3, 3};
    dstd::Vector<std::int64_t> v{1, 2, 3, 4};
    Series sk = Series::flat(TypeId::Uint64, k.data(), 4);
    Series sv = Series::flat_i64(v.data(), 4);
    dstd::Vector<const Series*> keys{&sk};
    dstd::Vector<const Series*> values{&sv};
    DataFrame r = dftu_utils_test::ok(
        dataframe::group_agg(keys, values, {AggSpec{AggOp::Sum, 0, "sum"}},
                             dstd::Vector<dstd::String>{"k"}));
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
    dstd::Vector<double> k{1.5, 1.5, 2.5, 2.5};
    dstd::Vector<std::int64_t> v{1, 2, 3, 4};
    Series sk = Series::flat_f64(k.data(), 4);
    Series sv = Series::flat_i64(v.data(), 4);
    dstd::Vector<const Series*> keys{&sk};
    dstd::Vector<const Series*> values{&sv};
    DataFrame r = dftu_utils_test::ok(
        dataframe::group_agg(keys, values, {AggSpec{AggOp::Count, -1, "cnt"}},
                             dstd::Vector<dstd::String>{"k"}));
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
    auto fine = dftu_utils_test::ok(
        dataframe::group_agg_state(s.keys2, s.values, mixed_specs()));

    auto coarse = dataframe::agg_regroup(*fine, {0});  // keep k1 only
    dataframe::agg_sort_groups(*coarse);
    DataFrame got = dataframe::agg_finalize(*coarse, "k1");

    auto direct_st = dftu_utils_test::ok(
        dataframe::group_agg_state(s.keys1, s.values, mixed_specs()));
    dataframe::agg_sort_groups(*direct_st);
    DataFrame want = dataframe::agg_finalize(*direct_st, "k1");

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
        CHECK(dstd::String(got.column("tags").string_at(r)) ==
              dstd::String(want.column("tags").string_at(r)));
        CHECK(dstd::String(got.column("argmax").string_at(r)) ==
              dstd::String(want.column("argmax").string_at(r)));
    }
    // Hist survives regroup as a list column with a row per group.
    CHECK(got.column("hist").length() == want.column("hist").length());
}

TEST_CASE("agg_regroup preserves occupancy busy/active exactly") {
    // ts/dur events across two keys; coarsen to one key and compare busy/active
    // against a direct single-key occupancy aggregation.
    const std::int64_t n = 2000;
    dstd::Vector<std::int64_t> k1(n), k2(n), ts(n), dur(n);
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
    dstd::Vector<const Series*> keys2{&sk1, &sk2};
    dstd::Vector<const Series*> keys1{&sk1};
    dstd::Vector<const Series*> values{&sts, &sdur};
    dstd::Vector<AggSpec> specs{AggSpec{AggOp::Busy, 0, "busy", 0.0, 1},
                                AggSpec{AggOp::Active, 0, "active", 0.0, 1}};

    auto fine =
        dftu_utils_test::ok(dataframe::group_agg_state(keys2, values, specs));
    auto coarse = dataframe::agg_regroup(*fine, {0});
    dataframe::agg_sort_groups(*coarse);
    DataFrame got = dataframe::agg_finalize(*coarse, "k1");

    auto direct =
        dftu_utils_test::ok(dataframe::group_agg_state(keys1, values, specs));
    dataframe::agg_sort_groups(*direct);
    DataFrame want = dataframe::agg_finalize(*direct, "k1");

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
    dstd::Vector<std::int64_t> bucket(n), grp(n), v(n);
    for (std::int64_t i = 0; i < n; ++i) {
        bucket[static_cast<std::size_t>(i)] = (i % 8) * 10;  // 0,10,...,70
        grp[static_cast<std::size_t>(i)] = i % 2;
        v[static_cast<std::size_t>(i)] = i;
    }
    Series sb = Series::flat_i64(bucket.data(), n);
    Series sg = Series::flat_i64(grp.data(), n);
    Series sv = Series::flat_i64(v.data(), n);
    dstd::Vector<const Series*> keys{&sb, &sg};
    dstd::Vector<const Series*> values{&sv};
    dstd::Vector<AggSpec> specs{AggSpec{AggOp::Sum, 0, "sum"},
                                AggSpec{AggOp::Count, -1, "cnt"}};

    auto fine =
        dftu_utils_test::ok(dataframe::group_agg_state(keys, values, specs));
    auto coarse = dataframe::agg_regroup(*fine, {0, 1}, /*bucket_recut=*/20);
    dataframe::agg_sort_groups(*coarse);
    DataFrame got = dataframe::agg_finalize(
        *coarse, dstd::Vector<dstd::String>{"bucket", "grp"});

    // Direct: floor each bucket to grain 20 up front, then group.
    dstd::Vector<std::int64_t> cb(n);
    for (std::int64_t i = 0; i < n; ++i)
        cb[static_cast<std::size_t>(i)] =
            (bucket[static_cast<std::size_t>(i)] / 20) * 20;
    Series scb = Series::flat_i64(cb.data(), n);
    dstd::Vector<const Series*> ckeys{&scb, &sg};
    auto direct =
        dftu_utils_test::ok(dataframe::group_agg_state(ckeys, values, specs));
    dataframe::agg_sort_groups(*direct);
    DataFrame want = dataframe::agg_finalize(
        *direct, dstd::Vector<dstd::String>{"bucket", "grp"});

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
dstd::Vector<AggDynSpec> dyn_specs() {
    return {{AggOp::Sum, 0.0, "sum_"},
            {AggOp::Mean, 0.0, "mean_"},
            {AggOp::Count, 0.0, "count_"}};
}

// One key column {0,0,1,1} plus two Float64 dyn args over rows [0,4).
struct DynSample {
    dstd::Vector<std::int64_t> k{0, 0, 1, 1};
    dstd::Vector<double> x{1, 2, 3, 4};
    dstd::Vector<double> y{10, 20, 30, 40};
    Series sk = Series::flat_i64(k.data(), 4);
    Series sx = Series::flat_f64(x.data(), 4);
    Series sy = Series::flat_f64(y.data(), 4);
    dstd::Vector<const Series*> keys{&sk};
};

}  // namespace

TEST_CASE("AggState dyn: per-batch name discovery unions into each group") {
    DynSample s;
    auto st = dataframe::agg_new({AggSpec{AggOp::Count, -1, "n"}}, dyn_specs());
    // Two batches over the same groups discover disjoint arg names (x then y);
    // each group's dyn set is the union, and the finalized columns are sorted.
    dftu_utils_test::ok(agg_accumulate(
        *st, s.keys, {}, dstd::Vector<AggDynInput>{{"x", &s.sx}}));
    dftu_utils_test::ok(agg_accumulate(
        *st, s.keys, {}, dstd::Vector<AggDynInput>{{"y", &s.sy}}));
    dataframe::agg_sort_groups(*st);
    DataFrame r = dataframe::agg_finalize(*st, "k");

    REQUIRE(r.num_rows() == 2);
    const dstd::Vector<dstd::String> expect{
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
    dstd::Vector<std::int64_t> k{0, 0, 1, 1};
    dstd::Vector<std::int64_t> z{5, 7, 11, 13};
    Series sk = Series::flat_i64(k.data(), 4);
    Series sz = Series::flat_i64(z.data(), 4);
    dstd::Vector<const Series*> keys{&sk};
    auto st = dataframe::agg_new({AggSpec{AggOp::Count, -1, "n"}},
                                 {{AggOp::Sum, 0.0, "sum_"}});
    dftu_utils_test::ok(
        agg_accumulate(*st, keys, {}, dstd::Vector<AggDynInput>{{"z", &sz}}));
    dataframe::agg_sort_groups(*st);
    DataFrame r = dataframe::agg_finalize(*st, "k");
    CHECK(r.column("sum_z").type() == TypeId::Int64);
    CHECK(r.column("sum_z").data<std::int64_t>()[0] == 12);
    CHECK(r.column("sum_z").data<std::int64_t>()[1] == 24);
}

TEST_CASE("AggState dyn: merge unions disjoint and overlapping names") {
    DynSample s;
    // State A sees x and y; state B sees y and w, in the same groups.
    dstd::Vector<double> w{100, 200, 300, 400};
    Series sw = Series::flat_f64(w.data(), 4);
    auto a = dataframe::agg_new({AggSpec{AggOp::Count, -1, "n"}}, dyn_specs());
    dftu_utils_test::ok(agg_accumulate(
        *a, s.keys, {}, dstd::Vector<AggDynInput>{{"x", &s.sx}, {"y", &s.sy}}));
    auto b = dataframe::agg_new({AggSpec{AggOp::Count, -1, "n"}}, dyn_specs());
    dftu_utils_test::ok(agg_accumulate(
        *b, s.keys, {}, dstd::Vector<AggDynInput>{{"y", &s.sy}, {"w", &sw}}));
    dataframe::agg_merge(*a, *b);
    dataframe::agg_sort_groups(*a);
    DataFrame r = dataframe::agg_finalize(*a, "k");

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
    auto st = dataframe::agg_new({AggSpec{AggOp::Count, -1, "n"}}, dyn_specs());
    dftu_utils_test::ok(
        agg_accumulate(*st, s.keys, {},
                       dstd::Vector<AggDynInput>{{"x", &s.sx}, {"y", &s.sy}}));
    dataframe::agg_sort_groups(*st);
    DataFrame want = dataframe::agg_finalize(*st, "k");

    dstd::String blob = dataframe::agg_serialize(*st);
    auto back = dftu_utils_test::ok(dataframe::agg_deserialize(blob));
    DataFrame got = dataframe::agg_finalize(*back, "k");

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
    dstd::Vector<std::int64_t> k(1000, 0);
    dstd::Vector<double> x(1000);
    for (std::int64_t i = 0; i < 1000; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<double>(i);
    Series sk = Series::flat_i64(k.data(), 1000);
    Series sx = Series::flat_f64(x.data(), 1000);
    dstd::Vector<const Series*> keys{&sk};
    auto st = dataframe::agg_new({AggSpec{AggOp::Count, -1, "n"}},
                                 {{AggOp::Pct, 0.9, "p90_"}});
    dftu_utils_test::ok(
        agg_accumulate(*st, keys, {}, dstd::Vector<AggDynInput>{{"x", &sx}}));
    DataFrame direct = dataframe::agg_finalize(*st, "k");
    const double q = direct.column("p90_x").data<double>()[0];
    CHECK(q == doctest::Approx(900.0).epsilon(0.02));

    auto back = dftu_utils_test::ok(
        dataframe::agg_deserialize(dataframe::agg_serialize(*st)));
    DataFrame r = dataframe::agg_finalize(*back, "k");
    CHECK(r.column("p90_x").data<double>()[0] == doctest::Approx(q));
}

TEST_CASE("splitting groups into zero parts ends the process" *
          doctest::skip()) {
    fresh_process::announce_abort();
    Sample s(64);
    auto st = dftu_utils_test::ok(
        dataframe::group_agg_state(s.keys1, s.values, mixed_specs()));
    (void)dataframe::agg_split_groups(*st, 0, 0);
}

TEST_CASE("splitting groups into zero parts is fatal") {
    auto result =
        fresh_process::run("splitting groups into zero parts ends the process");
    CHECK(result.aborted_with("agg_split_groups: parts must be > 0"));
}

TEST_CASE("splitting groups yields the requested part count") {
    Sample s(64);
    auto st = dftu_utils_test::ok(
        dataframe::group_agg_state(s.keys1, s.values, mixed_specs()));
    CHECK(dataframe::agg_split_groups(*st, 3, 0).size() == 3);
}

TEST_CASE("a blob read in the other format is an error, not misread bytes") {
    Sample s(64);
    auto st = dftu_utils_test::ok(
        dataframe::group_agg_state(s.keys1, s.values, mixed_specs()));
    dataframe::agg_sort_groups(*st);
    auto compact_as_full =
        dataframe::agg_deserialize(dataframe::agg_serialize(*st, true), false);
    REQUIRE_FALSE(compact_as_full.has_value());
    CHECK(common::errorcode_of(compact_as_full.error()) ==
          common::ErrorCode::Parse);
    auto full_as_compact =
        dataframe::agg_deserialize(dataframe::agg_serialize(*st), true);
    REQUIRE_FALSE(full_as_compact.has_value());
    CHECK(common::errorcode_of(full_as_compact.error()) ==
          common::ErrorCode::Parse);
}

TEST_CASE("a truncated or corrupt blob is an invalid argument") {
    Sample s(64);
    auto st = dftu_utils_test::ok(
        dataframe::group_agg_state(s.keys1, s.values, mixed_specs()));
    dataframe::agg_sort_groups(*st);
    for (const bool compact : {false, true}) {
        const dstd::String blob = dataframe::agg_serialize(*st, compact);
        for (const std::size_t keep : {std::size_t{4}, std::size_t{7},
                                       blob.size() / 2, blob.size() - 1}) {
            auto cut = dataframe::agg_deserialize(
                dstd::String(blob.data(), keep), compact);
            REQUIRE_FALSE(cut.has_value());
            CHECK(common::errorcode_of(cut.error()) ==
                  common::ErrorCode::InvalidArgument);
        }
        CHECK(dataframe::agg_deserialize(blob, compact).has_value());
    }
}

TEST_CASE(
    "compact serialization round-trips like the full one, in fewer bytes") {
    Sample s(4096);
    auto st = dftu_utils_test::ok(
        dataframe::group_agg_state(s.keys1, s.values, mixed_specs()));
    dataframe::agg_sort_groups(*st);
    const dstd::String full = dataframe::agg_serialize(*st);
    const dstd::String compact = dataframe::agg_serialize(*st, true);
    CHECK(compact.size() < full.size());

    DataFrame a = dataframe::agg_finalize(
        *dftu_utils_test::ok(dataframe::agg_deserialize(full)), "k1");
    DataFrame b = dataframe::agg_finalize(
        *dftu_utils_test::ok(dataframe::agg_deserialize(compact, true)), "k1");
    REQUIRE(a.names == b.names);
    REQUIRE(a.num_rows() == b.num_rows());
    for (std::int64_t r = 0; r < a.num_rows(); ++r) {
        CHECK(a.column("sum").data<std::int64_t>()[r] ==
              b.column("sum").data<std::int64_t>()[r]);
        CHECK(a.column("cnt").data<std::int64_t>()[r] ==
              b.column("cnt").data<std::int64_t>()[r]);
        CHECK(a.column("mean").data<double>()[r] ==
              b.column("mean").data<double>()[r]);
        CHECK(a.column("p50").data<double>()[r] ==
              b.column("p50").data<double>()[r]);
        CHECK(dstd::String(a.column("tags").string_at(r)) ==
              dstd::String(b.column("tags").string_at(r)));
        CHECK(dstd::String(a.column("argmax").string_at(r)) ==
              dstd::String(b.column("argmax").string_at(r)));
    }
}

TEST_CASE("agg_regroup over serialize/deserialize round-trip is exact") {
    Sample s(3000);
    auto fine = dftu_utils_test::ok(
        dataframe::group_agg_state(s.keys2, s.values, mixed_specs()));
    // Persist each group as a single-group blob, then rebuild one merged state
    // (the rollup read path), and coarsen that.
    auto rebuilt = dataframe::agg_new(mixed_specs());
    const std::int64_t ng = dataframe::agg_group_count(*fine);
    for (std::int64_t g = 0; g < ng; ++g) {
        dstd::String blob =
            dataframe::agg_serialize(*dataframe::agg_extract_group(*fine, g));
        auto one = dftu_utils_test::ok(dataframe::agg_deserialize(blob));
        dataframe::agg_merge(*rebuilt, *one);
    }
    auto coarse = dataframe::agg_regroup(*rebuilt, {0});
    dataframe::agg_sort_groups(*coarse);
    DataFrame got = dataframe::agg_finalize(*coarse, "k1");

    auto direct = dftu_utils_test::ok(
        dataframe::group_agg_state(s.keys1, s.values, mixed_specs()));
    dataframe::agg_sort_groups(*direct);
    DataFrame want = dataframe::agg_finalize(*direct, "k1");

    REQUIRE(got.num_rows() == want.num_rows());
    for (std::int64_t r = 0; r < got.num_rows(); ++r) {
        CHECK(got.column("sum").data<std::int64_t>()[r] ==
              want.column("sum").data<std::int64_t>()[r]);
        CHECK(got.column("p50").data<double>()[r] ==
              doctest::Approx(want.column("p50").data<double>()[r]));
        CHECK(dstd::String(got.column("tags").string_at(r)) ==
              dstd::String(want.column("tags").string_at(r)));
    }
}

namespace {

// The dyn side-table has two cell layouts, the light record (taken when the
// first batch carries the dynamics) and the full FieldStat per cell (taken when
// the state holds the group already). Every reduction must answer the same from
// either.
dataframe::AggStatePtr dyn_light_state(
    const dstd::Vector<AggDynSpec>& specs,
    const dstd::Vector<const Series*>& keys,
    const dstd::Vector<AggDynInput>& inputs) {
    auto st = dataframe::agg_new({}, specs);
    dftu_utils_test::ok(agg_accumulate(*st, keys, {}, inputs));
    return st;
}

dataframe::AggStatePtr dyn_full_state(const dstd::Vector<AggDynSpec>& specs,
                                      const dstd::Vector<const Series*>& keys,
                                      const dstd::Vector<AggDynInput>& inputs) {
    // The same batches regrouped into a fresh state, whose cells are FieldStats
    // unpacked from the light ones.
    auto st = dyn_light_state(specs, keys, inputs);
    return dataframe::agg_regroup(*st, {0});
}

// Every column, bit for bit. The two layouts must not differ in one bit.
void check_same_columns(const DataFrame& a, const DataFrame& b) {
    REQUIRE(a.names == b.names);
    REQUIRE(a.num_rows() == b.num_rows());
    for (const dstd::String& name : a.names) {
        const Series ca = a.column(name);
        const Series cb = b.column(name);
        REQUIRE(ca.type() == cb.type());
        for (std::int64_t r = 0; r < a.num_rows(); ++r) {
            const bool null_a = ca.is_null(r);
            CHECK(null_a == cb.is_null(r));
            if (null_a) continue;
            if (ca.type() == TypeId::Int64) {
                CHECK(ca.data<std::int64_t>()[r] == cb.data<std::int64_t>()[r]);
            } else if (ca.type() == TypeId::Float64) {
                CHECK(std::bit_cast<std::uint64_t>(ca.data<double>()[r]) ==
                      std::bit_cast<std::uint64_t>(cb.data<double>()[r]));
            } else {
                CHECK(dstd::String(ca.string_at(r)) ==
                      dstd::String(cb.string_at(r)));
            }
        }
    }
}

void check_same_dyn(const dstd::Vector<AggDynSpec>& specs,
                    const dstd::Vector<const Series*>& keys,
                    const dstd::Vector<AggDynInput>& inputs) {
    auto light = dyn_light_state(specs, keys, inputs);
    auto full = dyn_full_state(specs, keys, inputs);
    dataframe::agg_sort_groups(*light);
    dataframe::agg_sort_groups(*full);
    check_same_columns(dataframe::agg_finalize(*light, "k"),
                       dataframe::agg_finalize(*full, "k"));
}

// k = {0,0,0,0,1,1,2,2}, a: Int64 with rows 1 and 4 null, b: Float64 with row
// 7 null, so one group holds four values of each and the higher moments are not
// all zero. Every dyn reduction reads both.
struct DynData {
    dstd::Vector<std::int64_t> k{0, 0, 0, 0, 1, 1, 2, 2};
    dstd::Vector<std::int64_t> a{5, -3, 7, 11, 13, 17, 19, 23};
    dstd::Vector<double> b{1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5};
    std::uint8_t a_ok[1] = {0xED};  // rows 1 and 4 null
    std::uint8_t b_ok[1] = {0x7F};  // row 7 null
    Series sk = Series::flat_i64(k.data(), 8);
    Series sa = Series::flat_i64(a.data(), 8, a_ok);
    Series sb = Series::flat_f64(b.data(), 8, b_ok);
    dstd::Vector<const Series*> keys{&sk};
};

}  // namespace

TEST_CASE("AggState dyn: the light cells finalize like the full ones") {
    DynData d;
    check_same_dyn({{AggOp::Sum, 0.0, "sum_"}, {AggOp::Count, 0.0, "n_"}},
                   d.keys, {{"a", &d.sa}, {"b", &d.sb}});
    check_same_dyn({{AggOp::Mean, 0.0, "mean_"},
                    {AggOp::Min, 0.0, "min_"},
                    {AggOp::Max, 0.0, "max_"},
                    {AggOp::Std, 0.0, "std_"},
                    {AggOp::Var, 0.0, "var_"}},
                   d.keys, {{"a", &d.sa}, {"b", &d.sb}});
    // Pct answers from its sketch, which the light record does not touch.
    check_same_dyn({{AggOp::Pct, 0.9, "p90_"}}, d.keys,
                   {{"a", &d.sa}, {"b", &d.sb}});

    // The values themselves. Group 0 is a = {5, null, 7, 11} (three present,
    // sum 23) and b = {1.5, 2.5, 3.5, 4.5}.
    auto st =
        dyn_light_state({{AggOp::Sum, 0.0, "sum_"}, {AggOp::Count, 0.0, "n_"}},
                        d.keys, {{"a", &d.sa}, {"b", &d.sb}});
    dataframe::agg_sort_groups(*st);
    DataFrame r = dataframe::agg_finalize(*st, "k");
    CHECK(r.column("sum_a").type() == TypeId::Int64);
    CHECK(r.column("sum_a").data<std::int64_t>()[0] == 23);
    CHECK(r.column("sum_a").data<std::int64_t>()[1] == 17);
    CHECK(r.column("sum_a").data<std::int64_t>()[2] == 42);
    CHECK(r.column("n_a").data<std::int64_t>()[0] == 3);
    CHECK(r.column("n_a").data<std::int64_t>()[1] == 1);
    CHECK(r.column("n_a").data<std::int64_t>()[2] == 2);
    CHECK(r.column("sum_b").data<double>()[0] == doctest::Approx(12.0));
    CHECK(r.column("n_b").data<std::int64_t>()[0] == 4);
    CHECK(r.column("sum_b").data<double>()[1] == doctest::Approx(12.0));
    CHECK(r.column("n_b").data<std::int64_t>()[1] == 2);
    CHECK(r.column("sum_b").data<double>()[2] == doctest::Approx(7.5));
    CHECK(r.column("n_b").data<std::int64_t>()[2] == 1);
}

TEST_CASE("AggState dyn: an Int64 sum stays exact through the light cells") {
    // Above 2^53 the double route and the exact one part company, and the exact
    // sum overflows, which the light record must report like a FieldStat does.
    const std::int64_t big = std::numeric_limits<std::int64_t>::max();
    dstd::Vector<std::int64_t> k{0, 0, 0, 1, 1, 1};
    dstd::Vector<std::int64_t> a{big - 4, 4, 0, big, big, 1};
    Series sk = Series::flat_i64(k.data(), 6);
    Series sa = Series::flat_i64(a.data(), 6);
    dstd::Vector<const Series*> keys{&sk};
    const dstd::Vector<AggDynInput> in{{"a", &sa}};
    const dstd::Vector<AggDynSpec> specs{{AggOp::Sum, 0.0, "sum_"},
                                         {AggOp::Mean, 0.0, "mean_"}};
    check_same_dyn(specs, keys, in);

    auto st = dyn_light_state(specs, keys, in);
    dataframe::agg_sort_groups(*st);
    DataFrame r = dataframe::agg_finalize(*st, "k");
    // Group 0 sums to big exactly. Group 1 wraps, so its sum column is null.
    CHECK(!r.column("sum_a").is_null(0));
    CHECK(r.column("sum_a").data<std::int64_t>()[0] == big);
    CHECK(r.column("sum_a").is_null(1));
}

TEST_CASE("AggState dyn: a name fed two domains keeps the full cells") {
    // The light words are read in a name's first domain, so a later batch in
    // another one settles the side-table into full cells. The sum stays the
    // exact Int64 one either way.
    dstd::Vector<std::int64_t> k{0, 0, 1, 1};
    dstd::Vector<std::int64_t> ai{1, 2, 3, 4};
    dstd::Vector<double> af{10.5, 20.5, 30.5, 40.5};
    Series sk = Series::flat_i64(k.data(), 4);
    Series sa = Series::flat_i64(ai.data(), 4);
    Series sf = Series::flat_f64(af.data(), 4);
    dstd::Vector<const Series*> keys{&sk};
    const dstd::Vector<AggDynSpec> specs{{AggOp::Sum, 0.0, "sum_"},
                                         {AggOp::Count, 0.0, "n_"}};

    auto light = dataframe::agg_new({}, specs);
    dftu_utils_test::ok(agg_accumulate(*light, keys, {}, {{"a", &sa}}));
    dftu_utils_test::ok(agg_accumulate(*light, keys, {}, {{"a", &sf}}));
    auto full = dataframe::agg_new({}, specs);
    dftu_utils_test::ok(agg_accumulate(*full, keys, {}));
    dftu_utils_test::ok(agg_accumulate(*full, keys, {}, {{"a", &sa}}));
    dftu_utils_test::ok(agg_accumulate(*full, keys, {}, {{"a", &sf}}));
    dataframe::agg_sort_groups(*light);
    dataframe::agg_sort_groups(*full);
    check_same_columns(dataframe::agg_finalize(*light, "k"),
                       dataframe::agg_finalize(*full, "k"));

    DataFrame r = dataframe::agg_finalize(*light, "k");
    // Every row of both batches counts, and the exact sum is the Int64 batch's.
    CHECK(r.column("n_a").data<std::int64_t>()[0] == 4);
    CHECK(r.column("sum_a").data<std::int64_t>()[0] == 3);
    CHECK(r.column("sum_a").data<std::int64_t>()[1] == 7);
}

TEST_CASE("AggState dyn: merge unions names through both layouts") {
    dstd::Vector<std::int64_t> k{0, 0, 1, 1};
    dstd::Vector<std::int64_t> a{1, 2, 3, 4};
    dstd::Vector<double> b{10.0, 20.0, 30.0, 40.0};
    Series sk = Series::flat_i64(k.data(), 4);
    Series sa = Series::flat_i64(a.data(), 4);
    Series sb = Series::flat_f64(b.data(), 4);
    dstd::Vector<const Series*> keys{&sk};
    const dstd::Vector<AggDynSpec> specs{{AggOp::Sum, 0.0, "sum_"},
                                         {AggOp::Count, 0.0, "n_"}};

    // Two batches, one naming a only and one naming a and b. Folding them into
    // one state is the merge's answer. a is in both, so it counts twice.
    auto ref = dataframe::agg_new({}, specs);
    dftu_utils_test::ok(agg_accumulate(*ref, keys, {}, {{"a", &sa}}));
    dftu_utils_test::ok(
        agg_accumulate(*ref, keys, {}, {{"a", &sa}, {"b", &sb}}));
    dataframe::agg_sort_groups(*ref);
    const DataFrame w = dataframe::agg_finalize(*ref, "k");
    CHECK(w.column("n_a").data<std::int64_t>()[0] == 4);
    CHECK(w.column("sum_a").data<std::int64_t>()[0] == 6);

    auto merged_light = dyn_light_state(specs, keys, {{"a", &sa}});
    auto other = dyn_light_state(specs, keys, {{"a", &sa}, {"b", &sb}});
    dataframe::agg_merge(*merged_light, *other);
    dataframe::agg_sort_groups(*merged_light);
    check_same_columns(dataframe::agg_finalize(*merged_light, "k"), w);

    auto into_full = dyn_full_state(specs, keys, {{"a", &sa}});
    dataframe::agg_merge(*into_full, *other);
    dataframe::agg_sort_groups(*into_full);
    check_same_columns(dataframe::agg_finalize(*into_full, "k"), w);

    // A full other, one pass of both names, into a fresh destination. The same
    // batches folded once into one state.
    auto one_pass = dataframe::agg_new({}, specs);
    dftu_utils_test::ok(
        agg_accumulate(*one_pass, keys, {}, {{"a", &sa}, {"b", &sb}}));
    dataframe::agg_sort_groups(*one_pass);
    const DataFrame w1 = dataframe::agg_finalize(*one_pass, "k");

    auto into_light = dataframe::agg_new({}, specs);
    auto full_other = dyn_full_state(specs, keys, {{"a", &sa}, {"b", &sb}});
    dataframe::agg_merge(*into_light, *full_other);
    dataframe::agg_sort_groups(*into_light);
    check_same_columns(dataframe::agg_finalize(*into_light, "k"), w1);
}

TEST_CASE("AggState dyn: the light cells round-trip a blob") {
    DynData d;
    const dstd::Vector<AggDynSpec> specs{{AggOp::Sum, 0.0, "sum_"},
                                         {AggOp::Mean, 0.0, "mean_"},
                                         {AggOp::Min, 0.0, "min_"},
                                         {AggOp::Max, 0.0, "max_"},
                                         {AggOp::Std, 0.0, "std_"}};
    auto st = dyn_light_state(specs, d.keys, {{"a", &d.sa}, {"b", &d.sb}});
    dataframe::agg_sort_groups(*st);
    const DataFrame want = dataframe::agg_finalize(*st, "k");
    const dstd::String blob = dataframe::agg_serialize(*st);
    check_same_columns(
        dataframe::agg_finalize(
            *dftu_utils_test::ok(dataframe::agg_deserialize(blob)), "k"),
        want);
    check_same_columns(
        dataframe::agg_finalize(*dftu_utils_test::ok(dataframe::agg_deserialize(
                                    dataframe::agg_serialize(*st, true), true)),
                                "k"),
        want);

    // A blob written while the cells were full reads back as full cells and
    // still agrees, and its tag is not the light one.
    auto full = dyn_full_state(specs, d.keys, {{"a", &d.sa}, {"b", &d.sb}});
    dataframe::agg_sort_groups(*full);
    const dstd::String fblob = dataframe::agg_serialize(*full);
    check_same_columns(
        dataframe::agg_finalize(
            *dftu_utils_test::ok(dataframe::agg_deserialize(fblob)), "k"),
        want);
}

TEST_CASE("AggState dyn: an op the dyn path cannot compute is refused") {
    auto bad = dataframe::agg_new(
        {}, {dataframe::AggDynSpec{dataframe::AggOp::CountValid, 0.0, "cv_"}});
    auto refused = dataframe::agg_accumulate(
        *bad, dstd::Vector<const Series*>{}, dstd::Vector<const Series*>{},
        dstd::Vector<dataframe::AggDynInput>{}, 0, 0);
    REQUIRE_FALSE(refused.has_value());
    CHECK(common::errorcode_of(refused.error()) ==
          common::ErrorCode::InvalidArgument);
    auto good = dataframe::agg_new(
        {}, {dataframe::AggDynSpec{dataframe::AggOp::Mean, 0.0, "mean_"}});
    dftu_utils_test::ok(dataframe::agg_accumulate(
        *good, dstd::Vector<const Series*>{}, dstd::Vector<const Series*>{},
        dstd::Vector<dataframe::AggDynInput>{}, 0, 0));
}

TEST_CASE("seeding an op with no seed form ends the process" *
          doctest::skip()) {
    fresh_process::announce_abort();
    auto st = dataframe::agg_new({AggSpec{AggOp::First, 0, "first"}});
    dataframe::agg_seed_group(*st, {"k"}, {}, 1, {});
}

TEST_CASE("seeding an op with no seed form is fatal") {
    auto result =
        fresh_process::run("seeding an op with no seed form ends the process");
    CHECK(result.aborted_with("has no FieldStat/sketch seed representation"));
}

TEST_CASE("packed accumulation of a shape that does not pack ends the process" *
          doctest::skip()) {
    fresh_process::announce_abort();
    const std::int64_t k[2] = {1, 2};
    const std::int32_t v[2] = {3, 4};
    const Series key = Series::flat_i64(k, 2);
    const Series val = Series::flat(TypeId::Int32, v, 2);
    auto st = dataframe::agg_new({AggSpec{AggOp::Sum, 0, "sum"}});
    dataframe::AggPacked shape;
    dataframe::AggPages pages;
    dataframe::agg_accumulate_packed(*st, {&key}, {&val}, shape, pages);
}

TEST_CASE("packed accumulation of a shape that does not pack is fatal") {
    auto result = fresh_process::run(
        "packed accumulation of a shape that does not pack ends the process");
    CHECK(result.aborted_with("not a packable shape"));
}

TEST_CASE(
    "a text aggregate over a nested column refuses only a cell it reads") {
    const std::int64_t k[2] = {1, 1};
    const std::int64_t by[2] = {5, 6};
    const std::int64_t leaf[2] = {7, 8};
    const Series key = Series::flat_i64(k, 2);
    const Series byc = Series::flat_i64(by, 2);
    const Series nested = Series::list(dstd::Vector<std::int32_t>{0, 1, 2},
                                       Series::flat_i64(leaf, 2));
    AggSpec distinct{AggOp::Distinct, 0, "d"};
    distinct.param = 8.0;
    AggSpec argmax{AggOp::ArgMax, 0, "m"};
    argmax.by_col = 1;
    for (const AggSpec& spec : {distinct, argmax}) {
        const dstd::Vector<const Series*> values{&nested, &byc};
        auto st = dataframe::agg_new({spec});
        auto refused = dataframe::agg_accumulate(*st, key, values, 0, 2);
        REQUIRE_FALSE(refused.has_value());
        CHECK(common::errorcode_of(refused.error()) ==
              common::ErrorCode::InvalidArgument);
        auto empty = dataframe::agg_new({spec});
        CHECK(dataframe::agg_accumulate(*empty, key, values, 0, 0).has_value());
    }
}
