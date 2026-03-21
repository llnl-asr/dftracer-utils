#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace dftracer::utils::utilities::composites::dft::indexing;

TEST_SUITE("ChunkStatistics") {
    TEST_CASE("ChunkStatistics - Single event update") {
        ChunkStatistics stats;

        stats.update_from_event("read", "POSIX", 1000, 2000, 5000000, 100);

        CHECK(stats.total_events == 1);
        CHECK(stats.category_counts["POSIX"] == 1);
        CHECK(stats.name_counts["read"] == 1);
        CHECK(stats.pid_tid_counts["1000:2000"] == 1);
        CHECK(stats.min_timestamp_us == 5000000);
        CHECK(stats.max_timestamp_us == 5000100);  // ts + dur
        CHECK(stats.duration_count == 1);
        CHECK(stats.duration_sum_us == 100);
        CHECK(stats.duration_min_us == 100);
        CHECK(stats.duration_max_us == 100);
    }

    TEST_CASE("ChunkStatistics - Multiple events") {
        ChunkStatistics stats;

        stats.update_from_event("read", "POSIX", 1000, 2000, 1000, 100);
        stats.update_from_event("write", "POSIX", 1000, 2000, 2000, 200);
        stats.update_from_event("read", "storage", 1001, 2001, 3000, 300);

        CHECK(stats.total_events == 3);
        CHECK(stats.category_counts["POSIX"] == 2);
        CHECK(stats.category_counts["storage"] == 1);
        CHECK(stats.name_counts["read"] == 2);
        CHECK(stats.name_counts["write"] == 1);
        CHECK(stats.pid_tid_counts["1000:2000"] == 2);
        CHECK(stats.pid_tid_counts["1001:2001"] == 1);
        CHECK(stats.min_timestamp_us == 1000);
        CHECK(stats.max_timestamp_us == 3300);  // 3000 + 300
        CHECK(stats.duration_count == 3);
        CHECK(stats.duration_min_us == 100);
        CHECK(stats.duration_max_us == 300);
    }

    TEST_CASE("ChunkStatistics - Duration stats (Welford's)") {
        ChunkStatistics stats;

        // Add events with known durations: 10, 20, 30, 40, 50
        // mean = 30, variance = 250 (sample)
        stats.update_from_event("op", "cat", 1, 1, 1000, 10);
        stats.update_from_event("op", "cat", 1, 1, 2000, 20);
        stats.update_from_event("op", "cat", 1, 1, 3000, 30);
        stats.update_from_event("op", "cat", 1, 1, 4000, 40);
        stats.update_from_event("op", "cat", 1, 1, 5000, 50);

        CHECK(stats.duration_count == 5);
        CHECK(stats.duration_sum_us == 150);
        CHECK(doctest::Approx(stats.duration_mean()) == 30.0);
        CHECK(doctest::Approx(stats.duration_variance()) == 250.0);
    }

    TEST_CASE("ChunkStatistics - Merge correctness") {
        ChunkStatistics a;
        ChunkStatistics b;

        a.update_from_event("read", "POSIX", 1, 1, 1000, 100);
        a.update_from_event("read", "POSIX", 1, 1, 2000, 200);

        b.update_from_event("write", "POSIX", 2, 2, 3000, 300);

        a.merge_from(b);

        CHECK(a.total_events == 3);
        CHECK(a.category_counts["POSIX"] == 3);
        CHECK(a.name_counts["read"] == 2);
        CHECK(a.name_counts["write"] == 1);
        CHECK(a.min_timestamp_us == 1000);
        CHECK(a.max_timestamp_us == 3300);
        CHECK(a.duration_count == 3);
        CHECK(a.duration_min_us == 100);
        CHECK(a.duration_max_us == 300);
        CHECK(a.duration_sum_us == 600);
    }

    TEST_CASE("ChunkStatistics - Merge empty stats") {
        ChunkStatistics a;
        ChunkStatistics b;

        a.update_from_event("read", "POSIX", 1, 1, 1000, 100);
        a.merge_from(b);

        CHECK(a.total_events == 1);
        CHECK(a.duration_count == 1);
    }
}
