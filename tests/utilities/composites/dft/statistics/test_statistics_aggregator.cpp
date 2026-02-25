#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "testing_utilities.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::composites::dft::statistics;

static void populate_test_bidx(const std::string& bidx_path,
                               const std::string& file_path) {
    BloomIndexDatabase bidx(bidx_path);
    bidx.init_schema();

    int fid = bidx.get_or_create_file_info(file_path, 12345);

    bidx.begin_transaction();

    // Chunk 0: 2 events
    {
        ChunkStatistics stats;
        stats.update_from_event("read", "POSIX", 1, 1, 1000, 100);
        stats.update_from_event("write", "POSIX", 1, 2, 2000, 200);
        queries::insert_chunk_statistics(bidx.db(), fid, 0, stats);
    }

    // Chunk 1: 1 event
    {
        ChunkStatistics stats;
        stats.update_from_event("open", "storage", 2, 1, 5000, 50);
        queries::insert_chunk_statistics(bidx.db(), fid, 1, stats);
    }

    // Chunk 2: 2 events
    {
        ChunkStatistics stats;
        stats.update_from_event("read", "POSIX", 1, 1, 8000, 300);
        stats.update_from_event("stat", "POSIX", 3, 1, 9000, 10);
        queries::insert_chunk_statistics(bidx.db(), fid, 2, stats);
    }

    bidx.commit_transaction();
}

TEST_SUITE("StatisticsAggregatorUtility") {
    TEST_CASE("Aggregator - Basic aggregation from 3 chunks") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_stats_agg").string();
        fs::create_directories(test_dir);

        std::string bidx_path = test_dir + "/test.pfw.gz.bidx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_bidx(bidx_path, file_path);

        StatisticsAggregatorUtility aggregator;
        StatisticsAggregatorInput input;
        input.file_path = file_path;
        input.bidx_path = bidx_path;

        auto result = aggregator.process(input).get();

        CHECK(result.success == true);
        CHECK(result.num_chunks == 3);
        CHECK(result.merged.total_events == 5);

        // Category counts
        CHECK(result.merged.category_counts["POSIX"] == 4);
        CHECK(result.merged.category_counts["storage"] == 1);

        // Name counts
        CHECK(result.merged.name_counts["read"] == 2);
        CHECK(result.merged.name_counts["write"] == 1);
        CHECK(result.merged.name_counts["open"] == 1);
        CHECK(result.merged.name_counts["stat"] == 1);

        // Timestamp ranges
        CHECK(result.merged.min_timestamp_us == 1000);
        CHECK(result.merged.max_timestamp_us == 9010);  // 9000 + 10

        // Duration stats
        CHECK(result.merged.duration_count == 5);
        CHECK(result.merged.duration_sum_us == 660);  // 100+200+50+300+10
        CHECK(result.merged.duration_min_us == 10);
        CHECK(result.merged.duration_max_us == 300);

        // Mean: 660/5 = 132
        CHECK(result.duration_mean_us() == doctest::Approx(132.0));

        fs::remove_all(test_dir);
    }

    TEST_CASE("Aggregator - Missing bidx file") {
        StatisticsAggregatorUtility aggregator;
        StatisticsAggregatorInput input;
        input.file_path = "/fake/nonexistent.pfw.gz";
        input.bidx_path =
            dft_utils_test::make_unique_test_path("nonexistent").string() +
            ".bidx";

        auto result = aggregator.process(input).get();

        CHECK(result.success == false);
        CHECK(result.error_message.find("not found") != std::string::npos);
    }

    TEST_CASE("Aggregator - File not in bidx") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_stats_agg_notfound")
                .string();
        fs::create_directories(test_dir);

        std::string bidx_path = test_dir + "/test.pfw.gz.bidx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_bidx(bidx_path, file_path);

        StatisticsAggregatorUtility aggregator;
        StatisticsAggregatorInput input;
        input.file_path = "/fake/other_file.pfw.gz";
        input.bidx_path = bidx_path;

        auto result = aggregator.process(input).get();

        CHECK(result.success == false);
        CHECK(result.error_message.find("not found") != std::string::npos);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Aggregator - Empty chunk_statistics") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_stats_agg_empty")
                .string();
        fs::create_directories(test_dir);

        std::string bidx_path = test_dir + "/test.pfw.gz.bidx";
        std::string file_path = "/fake/test.pfw.gz";

        // Create bidx with file_info but no chunk_statistics
        BloomIndexDatabase bidx(bidx_path);
        bidx.init_schema();
        bidx.get_or_create_file_info(file_path, 12345);

        StatisticsAggregatorUtility aggregator;
        StatisticsAggregatorInput input;
        input.file_path = file_path;
        input.bidx_path = bidx_path;

        auto result = aggregator.process(input).get();

        CHECK(result.success == true);
        CHECK(result.num_chunks == 0);
        CHECK(result.merged.total_events == 0);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Aggregator - Welford's variance correctness") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_stats_agg_welford")
                .string();
        fs::create_directories(test_dir);

        std::string bidx_path = test_dir + "/test.pfw.gz.bidx";
        std::string file_path = "/fake/test.pfw.gz";

        BloomIndexDatabase bidx(bidx_path);
        bidx.init_schema();
        int fid = bidx.get_or_create_file_info(file_path, 12345);

        bidx.begin_transaction();

        // Chunk 0: durations 10, 20
        {
            ChunkStatistics stats;
            stats.update_from_event("op", "cat", 1, 1, 1000, 10);
            stats.update_from_event("op", "cat", 1, 1, 2000, 20);
            queries::insert_chunk_statistics(bidx.db(), fid, 0, stats);
        }

        // Chunk 1: durations 30, 40, 50
        {
            ChunkStatistics stats;
            stats.update_from_event("op", "cat", 1, 1, 3000, 30);
            stats.update_from_event("op", "cat", 1, 1, 4000, 40);
            stats.update_from_event("op", "cat", 1, 1, 5000, 50);
            queries::insert_chunk_statistics(bidx.db(), fid, 1, stats);
        }

        bidx.commit_transaction();

        StatisticsAggregatorUtility aggregator;
        StatisticsAggregatorInput input;
        input.file_path = file_path;
        input.bidx_path = bidx_path;

        auto result = aggregator.process(input).get();

        CHECK(result.success == true);
        // Combined: {10, 20, 30, 40, 50}, mean=30, sample_variance=250
        CHECK(result.duration_mean_us() == doctest::Approx(30.0));
        double variance = result.merged.duration_variance();
        CHECK(variance == doctest::Approx(250.0));
        CHECK(result.duration_stddev_us() == doctest::Approx(std::sqrt(250.0)));

        fs::remove_all(test_dir);
    }
}
