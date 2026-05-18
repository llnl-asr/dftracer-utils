#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "testing_utilities.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::internal;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::composites::dft::statistics;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::IndexDatabaseWriterContext;
using dftracer::utils::utilities::indexer::internal::get_logical_path;

static void write_chunk(
    IndexDatabaseWriterContext& writer, int fid, std::uint64_t checkpoint_idx,
    ChunkStatistics& stats,
    const std::vector<std::pair<std::string, std::string>>& dim_values) {
    writer.insert_chunk_statistics(fid, checkpoint_idx, stats);

    std::unordered_map<std::string, ChunkDimensionStats> dim_stats;
    for (const auto& [dim, val] : dim_values) {
        auto& ds = dim_stats[dim];
        ds.dimension = dim;
        ds.value_type = "string";
        ds.observe(val);
    }
    for (const auto& [dim, ds] : dim_stats) {
        writer.insert_chunk_dimension_stats(fid, checkpoint_idx, ds);
    }
}

static void populate_test_db(const std::string& db_root,
                             const std::string& file_path) {
    IndexDatabase idx_db(db_root);
    auto writer = idx_db.begin_write();
    writer->init_schema();

    int fid =
        writer->get_or_create_file_info(get_logical_path(file_path), 12345);

    {
        ChunkStatistics stats;
        stats.update_from_event("read", "POSIX", 1, 1, 1000, 100);
        stats.update_from_event("write", "POSIX", 1, 2, 2000, 200);
        write_chunk(*writer, fid, 0, stats,
                    {{"cat", "POSIX"},
                     {"cat", "POSIX"},
                     {"name", "read"},
                     {"name", "write"},
                     {"pid_tid", "1:1"},
                     {"pid_tid", "1:2"}});
    }

    {
        ChunkStatistics stats;
        stats.update_from_event("open", "storage", 2, 1, 5000, 50);
        write_chunk(*writer, fid, 1, stats,
                    {{"cat", "storage"}, {"name", "open"}, {"pid_tid", "2:1"}});
    }

    {
        ChunkStatistics stats;
        stats.update_from_event("read", "POSIX", 1, 1, 8000, 300);
        stats.update_from_event("stat", "POSIX", 3, 1, 9000, 10);
        write_chunk(*writer, fid, 2, stats,
                    {{"cat", "POSIX"},
                     {"cat", "POSIX"},
                     {"name", "read"},
                     {"name", "stat"},
                     {"pid_tid", "1:1"},
                     {"pid_tid", "3:1"}});
    }

    writer->commit();
}

TEST_SUITE("StatisticsAggregatorUtility") {
    TEST_CASE("Aggregator - Basic aggregation from 3 chunks") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_stats_agg").string();
        fs::create_directories(test_dir);

        std::string db_root =
            determine_index_path(test_dir + "/test.pfw.gz", "");
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_db(db_root, file_path);

        StatisticsAggregatorUtility aggregator;
        StatisticsAggregatorInput input;
        input.file_path = file_path;
        input.index_path = db_root;

        auto result = aggregator.process(input).get();

        CHECK(result.success == true);
        CHECK(result.num_chunks == 3);
        CHECK(result.merged.total_events == 5);

        CHECK(result.merged.category_counts["POSIX"] == 4);
        CHECK(result.merged.category_counts["storage"] == 1);
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
        input.index_path =
            (dft_utils_test::make_unique_test_path("nonexistent") / ".dftindex")
                .string();

        auto result = aggregator.process(input).get();

        CHECK(result.success == false);
        CHECK(result.error_message.find("not found") != std::string::npos);
    }

    TEST_CASE("Aggregator - File not in bidx") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_stats_agg_notfound")
                .string();
        fs::create_directories(test_dir);

        std::string db_root =
            determine_index_path(test_dir + "/test.pfw.gz", "");
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_db(db_root, file_path);

        StatisticsAggregatorUtility aggregator;
        StatisticsAggregatorInput input;
        input.file_path = "/fake/other_file.pfw.gz";
        input.index_path = db_root;

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

        std::string db_root =
            determine_index_path(test_dir + "/test.pfw.gz", "");
        std::string file_path = "/fake/test.pfw.gz";

        IndexDatabase idx_db(db_root);
        {
            auto writer = idx_db.begin_write();
            writer->init_schema();
            writer->get_or_create_file_info(get_logical_path(file_path), 12345);
            writer->commit();
        }

        StatisticsAggregatorUtility aggregator;
        StatisticsAggregatorInput input;
        input.file_path = file_path;
        input.index_path = db_root;

        auto result = aggregator.process(input).get();

        // Trace file doesn't exist, so the streaming fallback
        // correctly reports failure.
        CHECK(result.success == false);
        CHECK(result.error_message.find("not found") != std::string::npos);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Aggregator - Welford's variance correctness") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_stats_agg_welford")
                .string();
        fs::create_directories(test_dir);

        std::string db_root =
            determine_index_path(test_dir + "/test.pfw.gz", "");
        std::string file_path = "/fake/test.pfw.gz";

        IndexDatabase idx_db(db_root);
        int fid;
        {
            auto writer = idx_db.begin_write();
            writer->init_schema();
            fid = writer->get_or_create_file_info(get_logical_path(file_path),
                                                  12345);

            {
                ChunkStatistics stats;
                stats.update_from_event("op", "cat", 1, 1, 1000, 10);
                stats.update_from_event("op", "cat", 1, 1, 2000, 20);
                writer->insert_chunk_statistics(fid, 0, stats);
            }

            {
                ChunkStatistics stats;
                stats.update_from_event("op", "cat", 1, 1, 3000, 30);
                stats.update_from_event("op", "cat", 1, 1, 4000, 40);
                stats.update_from_event("op", "cat", 1, 1, 5000, 50);
                writer->insert_chunk_statistics(fid, 1, stats);
            }

            writer->commit();
        }

        StatisticsAggregatorUtility aggregator;
        StatisticsAggregatorInput input;
        input.file_path = file_path;
        input.index_path = db_root;

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
