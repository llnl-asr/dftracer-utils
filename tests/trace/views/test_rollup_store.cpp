// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/cache/rollup_store.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdint>

using namespace dftracer::utils;

using namespace dftracer::utils::trace::views::detail;
using namespace dftracer::utils::index::cache;

namespace {

// A single-key AggState (group "grp") with Count + Sum(v) over `values`.
dataframe::AggStatePtr make_state(const dstd::Vector<double>& values) {
    dstd::Vector<dstd::String> keys(values.size(), "grp");
    dstd::Vector<double> vd = values;
    dataframe::Series key = dataframe::Series::strings(keys);
    dataframe::Series val = dataframe::Series::flat_f64(
        vd.data(), static_cast<std::int64_t>(vd.size()));
    dstd::Vector<const dataframe::Series*> vals{&val};
    return dftu_utils_test::ok(dataframe::group_agg_state(
        {&key}, vals,
        {dataframe::AggSpec{dataframe::AggOp::Count, -1, "n"},
         dataframe::AggSpec{dataframe::AggOp::Sum, 0, "total"}}));
}

// A multi-group AggState: row i belongs to group gkeys[i] with value values[i].
dataframe::AggStatePtr make_state_kv(const dstd::Vector<dstd::String>& gkeys,
                                     const dstd::Vector<double>& values) {
    dstd::Vector<dstd::String> keys = gkeys;
    dstd::Vector<double> vd = values;
    dataframe::Series key = dataframe::Series::strings(keys);
    dataframe::Series val = dataframe::Series::flat_f64(
        vd.data(), static_cast<std::int64_t>(vd.size()));
    dstd::Vector<const dataframe::Series*> vals{&val};
    return dftu_utils_test::ok(dataframe::group_agg_state(
        {&key}, vals,
        {dataframe::AggSpec{dataframe::AggOp::Count, -1, "n"},
         dataframe::AggSpec{dataframe::AggOp::Sum, 0, "total"}}));
}

}  // namespace

TEST_SUITE("RollupStore") {
    // The distributed reduce: two ranks' AggState partials are combined with
    // agg_merge, persisted as per-group blobs, and read back through a real
    // store into an equivalent state.
    TEST_CASE("agg_merge of rank partials persists and reads back") {
        const dstd::String dir = dstd::String(
            dftu_utils_test::make_unique_test_path("dftu_rollup_store_test")
                .str());
        dstd::fs::remove_all(dir).discard();

        auto db = open_rollup_db(dir, CacheStore::Mode::ReadWrite);
        REQUIRE(db);
        const std::uint64_t sig = 0xABCDEF12;

        auto a = make_state({10, 20, 30});         // grp: n=3 total=60
        auto b = make_state({5, 15, 25, 35, 45});  // grp: n=5 total=125
        dataframe::agg_merge(*a, *b);              // grp: n=8 total=185
        dftu_utils_test::ok(persist_rollup(*db, sig, /*rest_sig=*/0,
                                           /*time_bucket_us=*/0,
                                           /*group_by=*/{}, *a));

        CHECK(dftu_utils_test::ok(rollup_exists(*db, sig)));
        auto got = dftu_utils_test::ok(read_rollup(*db, sig));
        REQUIRE(got);
        dataframe::DataFrame r = dataframe::agg_finalize(*got, "grp");
        REQUIRE(r.num_rows() == 1);
        CHECK(r.column("n").data<std::int64_t>()[0] == 8);
        CHECK(r.column("total").data<double>()[0] == doctest::Approx(185));

        // A different signature is isolated and absent.
        CHECK_FALSE(dftu_utils_test::ok(rollup_exists(*db, 0x99999999)));
        CHECK(dftu_utils_test::ok(read_rollup(*db, 0x99999999)) == nullptr);

        dstd::fs::remove_all(dir).discard();
    }

    // Two ranks' partials over three distinct groups round-trip: every group's
    // per-group blob persists and reads back into an equivalent merged state.
    TEST_CASE("multiple groups persist and read back") {
        const dstd::String dir =
            dstd::String(dftu_utils_test::make_unique_test_path(
                             "dftu_rollup_store_multi_test")
                             .str());
        dstd::fs::remove_all(dir).discard();

        auto db = open_rollup_db(dir, CacheStore::Mode::ReadWrite);
        REQUIRE(db);
        const std::uint64_t sig = 0xBEEF01;

        auto a = make_state_kv({"g1", "g1", "g2"}, {10, 20, 100});
        auto b = make_state_kv({"g2", "g3"}, {50, 7});
        dataframe::agg_merge(*a,
                             *b);  // g1: n=2 t=30; g2: n=2 t=150; g3: n=1 t=7
        dftu_utils_test::ok(persist_rollup(*db, sig, /*rest_sig=*/0,
                                           /*time_bucket_us=*/0,
                                           /*group_by=*/{}, *a));

        CHECK(dftu_utils_test::ok(rollup_exists(*db, sig)));
        auto got = dftu_utils_test::ok(read_rollup(*db, sig));
        REQUIRE(got);
        dataframe::DataFrame r = dataframe::agg_finalize(*got, "grp");
        REQUIRE(r.num_rows() == 3);

        dstd::Map<dstd::String, dstd::Pair<std::int64_t, double>> by_group;
        for (std::int64_t i = 0; i < r.num_rows(); ++i)
            by_group[dstd::String(r.column("grp").string_at(i))] = {
                r.column("n").data<std::int64_t>()[i],
                r.column("total").data<double>()[i]};

        REQUIRE(by_group.count("g1"));
        REQUIRE(by_group.count("g2"));
        REQUIRE(by_group.count("g3"));
        CHECK(by_group["g1"].first == 2);
        CHECK(by_group["g1"].second == doctest::Approx(30));
        CHECK(by_group["g2"].first == 2);
        CHECK(by_group["g2"].second == doctest::Approx(150));
        CHECK(by_group["g3"].first == 1);
        CHECK(by_group["g3"].second == doctest::Approx(7));

        dstd::fs::remove_all(dir).discard();
    }
}
