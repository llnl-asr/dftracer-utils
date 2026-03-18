#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <doctest/doctest.h>

#include <string>

#include "testing_utilities.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;

TEST_SUITE("ManifestQueries") {
    TEST_CASE("Pack and unpack line numbers") {
        std::vector<std::uint32_t> lines = {0, 3, 7, 15, 42, 100};
        auto blob = queries::pack_line_numbers(lines);
        CHECK(blob.size() == lines.size() * sizeof(std::uint32_t));

        auto unpacked = queries::unpack_line_numbers(blob.data(), blob.size());
        CHECK(unpacked == lines);
    }

    TEST_CASE("Insert and query event ranges") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_manifest_queries")
                .string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";

        IndexDatabase idx_db(idx_path);
        idx_db.init_base_schema();
        idx_db.init_manifest_schema();
        int fid =
            idx_db.get_or_create_file_info(get_logical_path("test.pfw.gz"), 0);

        idx_db.begin_transaction();

        queries::insert_event_range(idx_db.sql_db(), fid, 0, "POSIX", "read",
                                    {0, 2, 5});
        queries::insert_event_range(idx_db.sql_db(), fid, 0, "POSIX", "write",
                                    {1});
        queries::insert_event_range(idx_db.sql_db(), fid, 0, "APP", "compute",
                                    {3, 4});
        queries::insert_event_range(idx_db.sql_db(), fid, 1, "POSIX", "read",
                                    {0, 1});

        idx_db.commit_transaction();

        auto all = queries::query_event_ranges(idx_db.sql_db(), fid);
        CHECK(all.size() == 4);

        auto ckpt0 =
            queries::query_event_ranges_for_checkpoint(idx_db.sql_db(), fid, 0);
        CHECK(ckpt0.size() == 3);

        auto ckpt1 =
            queries::query_event_ranges_for_checkpoint(idx_db.sql_db(), fid, 1);
        CHECK(ckpt1.size() == 1);
        CHECK(ckpt1[0].cat == "POSIX");
        CHECK(ckpt1[0].name == "read");
        CHECK(ckpt1[0].line_numbers.size() == 2);
        CHECK(ckpt1[0].event_count == 2);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Insert and query metadata lines") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_manifest_meta_q")
                .string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";

        IndexDatabase idx_db(idx_path);
        idx_db.init_base_schema();
        idx_db.init_manifest_schema();
        int fid =
            idx_db.get_or_create_file_info(get_logical_path("test.pfw.gz"), 0);

        idx_db.begin_transaction();

        queries::insert_metadata_lines(idx_db.sql_db(), fid, 0, "HH", {0, 3});
        queries::insert_metadata_lines(idx_db.sql_db(), fid, 0, "FH", {1});
        queries::insert_metadata_lines(idx_db.sql_db(), fid, 1, "HH", {0});

        idx_db.commit_transaction();

        auto all = queries::query_metadata_lines(idx_db.sql_db(), fid);
        CHECK(all.size() == 3);

        auto ckpt0 = queries::query_metadata_lines_for_checkpoint(
            idx_db.sql_db(), fid, 0);
        CHECK(ckpt0.size() == 2);

        auto ckpt1 = queries::query_metadata_lines_for_checkpoint(
            idx_db.sql_db(), fid, 1);
        CHECK(ckpt1.size() == 1);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Delete operations") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_manifest_delete")
                .string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";

        IndexDatabase idx_db(idx_path);
        idx_db.init_base_schema();
        idx_db.init_manifest_schema();
        int fid =
            idx_db.get_or_create_file_info(get_logical_path("test.pfw.gz"), 0);

        idx_db.begin_transaction();
        queries::insert_event_range(idx_db.sql_db(), fid, 0, "POSIX", "read",
                                    {0, 1});
        queries::insert_metadata_lines(idx_db.sql_db(), fid, 0, "HH", {2});
        idx_db.commit_transaction();

        CHECK(queries::query_event_ranges(idx_db.sql_db(), fid).size() == 1);
        CHECK(queries::query_metadata_lines(idx_db.sql_db(), fid).size() == 1);

        queries::delete_event_ranges(idx_db.sql_db(), fid);
        CHECK(queries::query_event_ranges(idx_db.sql_db(), fid).empty());

        queries::delete_metadata_lines(idx_db.sql_db(), fid);
        CHECK(queries::query_metadata_lines(idx_db.sql_db(), fid).empty());

        fs::remove_all(test_dir);
    }
}
