#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/views/rollup_store.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils::trace::views::detail;
namespace df = dftracer::utils::dataframe;

namespace {

// A single-key AggState (group "grp") with Count + Sum(v) over `values`.
df::AggStatePtr make_state(const std::vector<double>& values) {
    std::vector<std::string> keys(values.size(), "grp");
    std::vector<double> vd = values;
    df::Series key = df::Series::strings(keys);
    df::Series val =
        df::Series::flat_f64(vd.data(), static_cast<std::int64_t>(vd.size()));
    std::vector<const df::Series*> vals{&val};
    return df::group_agg_state({&key}, vals,
                               {df::AggSpec{df::AggOp::Count, -1, "n"},
                                df::AggSpec{df::AggOp::Sum, 0, "total"}});
}

// A multi-group AggState: row i belongs to group gkeys[i] with value values[i].
df::AggStatePtr make_state_kv(const std::vector<std::string>& gkeys,
                              const std::vector<double>& values) {
    std::vector<std::string> keys = gkeys;
    std::vector<double> vd = values;
    df::Series key = df::Series::strings(keys);
    df::Series val =
        df::Series::flat_f64(vd.data(), static_cast<std::int64_t>(vd.size()));
    std::vector<const df::Series*> vals{&val};
    return df::group_agg_state({&key}, vals,
                               {df::AggSpec{df::AggOp::Count, -1, "n"},
                                df::AggSpec{df::AggOp::Sum, 0, "total"}});
}

}  // namespace

TEST_SUITE("RollupStore") {
    TEST_CASE("key layout: tag, big-endian signature, group suffix") {
        const std::uint64_t sig = 0x0102030405060708ULL;

        std::string desc = rollup_desc_key(sig);
        REQUIRE(desc.size() == 9);
        CHECK(static_cast<unsigned char>(desc[0]) == 0x00);
        CHECK(static_cast<unsigned char>(desc[1]) == 0x01);  // big-endian
        CHECK(static_cast<unsigned char>(desc[8]) == 0x08);

        std::string row = rollup_row_key(sig, "abc");
        REQUIRE(row.size() == 12);
        CHECK(static_cast<unsigned char>(row[0]) == 0x01);
        CHECK(row.substr(9) == "abc");
        CHECK(row.substr(1, 8) == desc.substr(1, 8));  // same signature bytes
    }

    TEST_CASE("key ordering groups a view and separates descriptors") {
        const std::uint64_t s1 = 10, s2 = 11;
        CHECK(rollup_desc_key(s1) < rollup_row_key(s1, ""));
        CHECK(rollup_row_key(s1, "a") < rollup_row_key(s1, "b"));
        CHECK(rollup_row_key(s1, "z") < rollup_row_key(s2, "a"));
    }

    // The distributed reduce: two ranks' AggState partials are combined with
    // agg_merge, persisted as per-group blobs, and read back through a real
    // RocksDB into an equivalent state.
    TEST_CASE("agg_merge of rank partials persists and reads back") {
        namespace rdb = dftracer::utils::rocksdb;
        const std::string dir =
            dftu_utils_test::make_unique_test_path("dftu_rollup_store_test")
                .string();
        fs::remove_all(dir);
        rdb::RocksDBManager::instance().reset(dir);

        auto db = open_rollup_db(dir, rdb::RocksDatabase::OpenMode::ReadWrite);
        REQUIRE(db);
        const std::uint64_t sig = 0xABCDEF12;

        auto a = make_state({10, 20, 30});         // grp: n=3 total=60
        auto b = make_state({5, 15, 25, 35, 45});  // grp: n=5 total=125
        df::agg_merge(*a, *b);                     // grp: n=8 total=185
        persist_rollup(*db, sig, /*rest_sig=*/0, /*time_bucket_us=*/0,
                       /*group_by=*/{}, *a);

        CHECK(rollup_exists(*db, sig));
        auto got = read_rollup(*db, sig);
        REQUIRE(got);
        df::DataFrame r = df::agg_finalize(*got, "grp");
        REQUIRE(r.num_rows() == 1);
        CHECK(r.column("n").data<std::int64_t>()[0] == 8);
        CHECK(r.column("total").data<double>()[0] == doctest::Approx(185));

        // A different signature is isolated and absent.
        CHECK_FALSE(rollup_exists(*db, 0x99999999));
        CHECK(read_rollup(*db, 0x99999999) == nullptr);

        rdb::RocksDBManager::instance().reset(dir);
        fs::remove_all(dir);
    }

    // Two ranks' partials over three distinct groups round-trip: every group's
    // per-group blob persists and reads back into an equivalent merged state.
    TEST_CASE("multiple groups persist and read back") {
        namespace rdb = dftracer::utils::rocksdb;
        const std::string dir = dftu_utils_test::make_unique_test_path(
                                    "dftu_rollup_store_multi_test")
                                    .string();
        fs::remove_all(dir);
        rdb::RocksDBManager::instance().reset(dir);

        auto db = open_rollup_db(dir, rdb::RocksDatabase::OpenMode::ReadWrite);
        REQUIRE(db);
        const std::uint64_t sig = 0xBEEF01;

        auto a = make_state_kv({"g1", "g1", "g2"}, {10, 20, 100});
        auto b = make_state_kv({"g2", "g3"}, {50, 7});
        df::agg_merge(*a, *b);  // g1: n=2 t=30; g2: n=2 t=150; g3: n=1 t=7
        persist_rollup(*db, sig, /*rest_sig=*/0, /*time_bucket_us=*/0,
                       /*group_by=*/{}, *a);

        CHECK(rollup_exists(*db, sig));
        auto got = read_rollup(*db, sig);
        REQUIRE(got);
        df::DataFrame r = df::agg_finalize(*got, "grp");
        REQUIRE(r.num_rows() == 3);

        std::map<std::string, std::pair<std::int64_t, double>> by_group;
        for (std::int64_t i = 0; i < r.num_rows(); ++i)
            by_group[std::string(r.column("grp").string_at(i))] = {
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

        rdb::RocksDBManager::instance().reset(dir);
        fs::remove_all(dir);
    }
}
