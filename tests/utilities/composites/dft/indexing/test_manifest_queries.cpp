#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/indexing/manifest_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <doctest/doctest.h>

#include <string>

#include "testing_utilities.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::indexing;

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
        std::string midx_path = test_dir + "/test.pfw.gz.midx";

        ManifestIndexDatabase midx(midx_path);
        midx.init_schema();
        int fid = midx.get_or_create_file_info("test.pfw.gz", 0);

        midx.begin_transaction();

        queries::insert_event_range(midx.db(), fid, 0, "POSIX", "read",
                                    {0, 2, 5});
        queries::insert_event_range(midx.db(), fid, 0, "POSIX", "write", {1});
        queries::insert_event_range(midx.db(), fid, 0, "APP", "compute",
                                    {3, 4});
        queries::insert_event_range(midx.db(), fid, 1, "POSIX", "read", {0, 1});

        midx.commit_transaction();

        auto all = queries::query_event_ranges(midx.db(), fid);
        CHECK(all.size() == 4);

        auto ckpt0 =
            queries::query_event_ranges_for_checkpoint(midx.db(), fid, 0);
        CHECK(ckpt0.size() == 3);

        auto ckpt1 =
            queries::query_event_ranges_for_checkpoint(midx.db(), fid, 1);
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
        std::string midx_path = test_dir + "/test.pfw.gz.midx";

        ManifestIndexDatabase midx(midx_path);
        midx.init_schema();
        int fid = midx.get_or_create_file_info("test.pfw.gz", 0);

        midx.begin_transaction();

        queries::insert_metadata_lines(midx.db(), fid, 0, "HH", {0, 3});
        queries::insert_metadata_lines(midx.db(), fid, 0, "FH", {1});
        queries::insert_metadata_lines(midx.db(), fid, 1, "HH", {0});

        midx.commit_transaction();

        auto all = queries::query_metadata_lines(midx.db(), fid);
        CHECK(all.size() == 3);

        auto ckpt0 =
            queries::query_metadata_lines_for_checkpoint(midx.db(), fid, 0);
        CHECK(ckpt0.size() == 2);

        auto ckpt1 =
            queries::query_metadata_lines_for_checkpoint(midx.db(), fid, 1);
        CHECK(ckpt1.size() == 1);

        fs::remove_all(test_dir);
    }

    TEST_CASE("Delete operations") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_manifest_delete")
                .string();
        fs::create_directories(test_dir);
        std::string midx_path = test_dir + "/test.pfw.gz.midx";

        ManifestIndexDatabase midx(midx_path);
        midx.init_schema();
        int fid = midx.get_or_create_file_info("test.pfw.gz", 0);

        midx.begin_transaction();
        queries::insert_event_range(midx.db(), fid, 0, "POSIX", "read", {0, 1});
        queries::insert_metadata_lines(midx.db(), fid, 0, "HH", {2});
        midx.commit_transaction();

        CHECK(queries::query_event_ranges(midx.db(), fid).size() == 1);
        CHECK(queries::query_metadata_lines(midx.db(), fid).size() == 1);

        queries::delete_event_ranges(midx.db(), fid);
        CHECK(queries::query_event_ranges(midx.db(), fid).empty());

        queries::delete_metadata_lines(midx.db(), fid);
        CHECK(queries::query_metadata_lines(midx.db(), fid).empty());

        fs::remove_all(test_dir);
    }

    TEST_CASE("determine_manifest_index_path") {
        CHECK(determine_manifest_index_path("/data/trace.pfw.gz") ==
              "/data/trace.pfw.gz.midx");

        CHECK(determine_manifest_index_path("/data/trace.pfw.gz", "/index") ==
              "/index/trace.pfw.gz.midx");
    }
}
