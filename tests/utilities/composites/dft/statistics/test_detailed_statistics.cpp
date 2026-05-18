#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/statistics/detailed_statistics.h>
#include <doctest/doctest.h>
#include <simdjson.h>

#include <cmath>
#include <string>

using namespace dftracer::utils::utilities::composites::dft::statistics;

TEST_SUITE("DistributionStats") {
    TEST_CASE("update and count") {
        DistributionStats dist;
        dist.update(100);
        dist.update(200);
        dist.update(300);

        CHECK(dist.count() == 3);
        CHECK(dist.sum == doctest::Approx(600.0));
        CHECK(dist.mean() == doctest::Approx(200.0));
        CHECK(!dist.sketch.empty());
        REQUIRE(!dist.sketch.empty());
        CHECK(dist.sketch.count() == 3);
    }

    TEST_CASE("mean - empty") {
        DistributionStats dist;
        CHECK(dist.count() == 0);
        CHECK(dist.mean() == 0.0);
    }

    TEST_CASE("merge") {
        DistributionStats a, b;
        a.update(100);
        a.update(200);
        b.update(300);
        b.update(400);

        a.merge(b);

        CHECK(a.count() == 4);
        CHECK(a.sum == doctest::Approx(1000.0));
        CHECK(a.mean() == doctest::Approx(250.0));
    }

    TEST_CASE("merge - empty into non-empty") {
        DistributionStats a, empty;
        a.update(100);
        a.merge(empty);

        CHECK(a.count() == 1);
        CHECK(a.sum == doctest::Approx(100.0));
    }

    TEST_CASE("merge - non-empty into empty") {
        DistributionStats empty, b;
        b.update(100);
        empty.merge(b);

        CHECK(empty.count() == 1);
        CHECK(empty.sum == doctest::Approx(100.0));
    }
}

TEST_SUITE("IOEventMetrics") {
    TEST_CASE("merge") {
        IOEventMetrics a, b;
        a.duration.update(10);
        a.size.update(4096);
        a.bandwidth.update(1e9);

        b.duration.update(20);
        b.size.update(8192);
        b.bandwidth.update(2e9);

        a.merge(b);

        CHECK(a.duration.count() == 2);
        CHECK(a.size.count() == 2);
        CHECK(a.bandwidth.count() == 2);
        CHECK(a.size.sum == doctest::Approx(4096.0 + 8192.0));
    }
}

TEST_SUITE("DetailedStatistics") {
    TEST_CASE("global duration") {
        DetailedStatistics stats;
        stats.duration.update(100);
        stats.duration.update(200);
        stats.duration.update(300);

        CHECK(stats.duration.count() == 3);
        CHECK(!stats.duration.sketch.empty());
        REQUIRE(!stats.duration.sketch.empty());
        CHECK(stats.duration.sketch.count() == 3);
    }

    TEST_CASE("grouped duration") {
        DetailedStatistics stats;
        stats.duration.update(100);
        stats.duration.update(200);

        stats.grouped_duration["read"].update(100);
        stats.grouped_duration["write"].update(200);

        CHECK(stats.grouped_duration.size() == 2);
        CHECK(stats.grouped_duration["read"].count() == 1);
        CHECK(stats.grouped_duration["write"].count() == 1);
    }

    TEST_CASE("grouped I/O metrics") {
        DetailedStatistics stats;
        stats.grouped_io["read"].duration.update(10);
        stats.grouped_io["read"].size.update(4096);
        stats.grouped_io["read"].bandwidth.update(4096.0 * 1e6 / 10.0);

        CHECK(stats.grouped_io.size() == 1);
        CHECK(stats.grouped_io["read"].size.count() == 1);
        CHECK(stats.grouped_io["read"].size.sum == doctest::Approx(4096.0));
    }

    TEST_CASE("merge - basic") {
        DetailedStatistics a, b;
        a.duration.update(100);
        a.duration.update(200);
        a.grouped_duration["read"].update(100);
        a.grouped_io["read"].size.update(4096);
        a.events_scanned = 10;
        a.chunks_scanned = 1;
        a.chunks_skipped = 2;

        b.duration.update(300);
        b.grouped_duration["read"].update(300);
        b.grouped_duration["write"].update(500);
        b.grouped_io["read"].size.update(8192);
        b.grouped_io["write"].size.update(1024);
        b.events_scanned = 5;
        b.chunks_scanned = 1;
        b.chunks_skipped = 3;

        a.merge(b);

        CHECK(a.duration.count() == 3);
        REQUIRE(!a.duration.sketch.empty());
        CHECK(a.duration.sketch.count() == 3);
        CHECK(a.grouped_duration["read"].count() == 2);
        CHECK(a.grouped_duration["write"].count() == 1);
        CHECK(a.grouped_io["read"].size.count() == 2);
        CHECK(a.grouped_io["read"].size.sum ==
              doctest::Approx(4096.0 + 8192.0));
        CHECK(a.grouped_io["write"].size.count() == 1);
        CHECK(a.events_scanned == 15);
        CHECK(a.chunks_scanned == 2);
        CHECK(a.chunks_skipped == 5);
    }

    TEST_CASE("merge - empty into non-empty") {
        DetailedStatistics a, empty;
        a.duration.update(100);
        a.grouped_duration["read"].update(100);
        a.events_scanned = 10;

        a.merge(empty);

        CHECK(a.duration.count() == 1);
        CHECK(a.grouped_duration["read"].count() == 1);
        CHECK(a.events_scanned == 10);
    }

    TEST_CASE("merge - non-empty into empty") {
        DetailedStatistics empty, b;
        b.duration.update(100);
        b.grouped_duration["read"].update(100);
        b.events_scanned = 5;

        empty.merge(b);

        CHECK(empty.duration.count() == 1);
        CHECK(empty.grouped_duration["read"].count() == 1);
        CHECK(empty.events_scanned == 5);
    }

    TEST_CASE("to_json - produces valid JSON") {
        DetailedStatistics stats;
        stats.duration.update(100);
        stats.duration.update(200);
        stats.grouped_duration["read"].update(100);
        stats.grouped_io["read"].size.update(4096);
        stats.events_scanned = 2;
        stats.chunks_scanned = 1;
        stats.chunks_skipped = 3;

        std::string json = stats.to_json();

        simdjson::dom::parser parser;
        auto result = parser.parse(json);
        REQUIRE(!result.error());

        auto root = result.value_unsafe();
        REQUIRE(root.is_object());

        CHECK(root["events_scanned"].get_uint64().value() == 2);
        CHECK(root["chunks_scanned"].get_uint64().value() == 1);
        CHECK(root["chunks_skipped"].get_uint64().value() == 3);

        auto dur = root["duration"];
        REQUIRE(!dur.error());
        REQUIRE(dur.is_object());
        CHECK(dur["count"].get_uint64().value() == 2);

        auto gd = root["grouped_duration"];
        REQUIRE(!gd.error());
        REQUIRE(gd.is_object());
        auto gd_read = gd["read"];
        REQUIRE(!gd_read.error());
        REQUIRE(gd_read.is_object());
        CHECK(gd_read["count"].get_uint64().value() == 1);

        auto gio = root["grouped_io"];
        REQUIRE(!gio.error());
        REQUIRE(gio.is_object());
        auto gio_read = gio["read"];
        REQUIRE(!gio_read.error());
        REQUIRE(gio_read.is_object());
    }

    TEST_CASE("to_json - no grouped when empty") {
        DetailedStatistics stats;
        stats.duration.update(100);

        std::string json = stats.to_json();

        simdjson::dom::parser parser;
        auto result = parser.parse(json);
        REQUIRE(!result.error());

        auto root = result.value_unsafe();
        CHECK(root["grouped_duration"].error());
        CHECK(root["grouped_io"].error());
    }

    TEST_CASE("to_json - global duration always present") {
        DetailedStatistics stats;
        // Even with no events, duration section should be present
        std::string json = stats.to_json();

        simdjson::dom::parser parser;
        auto result = parser.parse(json);
        REQUIRE(!result.error());

        auto root = result.value_unsafe();
        auto dur = root["duration"];
        REQUIRE(!dur.error());
        REQUIRE(dur.is_object());
        CHECK(dur["count"].get_uint64().value() == 0);
    }
}
