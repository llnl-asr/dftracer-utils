#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/indexing/manifest_index_schema.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/manifest_queries.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reconstruction_planner.h>
#include <doctest/doctest.h>

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "testing_utilities.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::composites::dft::reorganize;

TEST_SUITE("ReconstructionPlanner") {
    TEST_CASE("Empty input produces empty plan") {
        ReconstructionPlannerUtility planner;
        ReconstructionPlannerInput input;
        // reorganized_files is empty by default

        auto plan = planner.process(input).get();

        CHECK(plan.files.empty());
        CHECK(plan.total_segments == 0);
        CHECK(plan.total_events == 0);
    }

    TEST_CASE("Single reorganized file with provenance") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_recon_planner")
                .string();
        fs::create_directories(test_dir);

        // Create a dummy reorganized file
        std::string reorg_file = test_dir + "/io.pfw.gz";
        {
            std::ofstream ofs(reorg_file);
            ofs << "dummy";
        }

        // Create .midx sidecar with provenance
        std::string midx_path = determine_manifest_index_path(reorg_file, "");
        {
            ManifestIndexDatabase midx(midx_path);
            midx.init_schema();
            midx.get_or_create_file_info(reorg_file, 0);

            midx.begin_transaction();

            // Provenance info
            queries::insert_provenance_info(midx.db(), "version", "1.0");
            queries::insert_provenance_info(midx.db(), "tool",
                                            "dftracer_organize");

            // Provenance group
            queries::insert_provenance_group(midx.db(), "io", "cat=POSIX");

            // Provenance source
            int fid = midx.get_file_info_id(reorg_file);
            queries::insert_provenance_source(
                midx.db(), fid, 0, "/original/trace.pfw.gz", 3, "abc123");

            // Provenance segments (3 checkpoints)
            queries::insert_provenance_segment(midx.db(), 0, 0, 0, 100, 100);
            queries::insert_provenance_segment(midx.db(), 0, 1, 100, 250, 150);
            queries::insert_provenance_segment(midx.db(), 0, 2, 250, 400, 150);

            midx.commit_transaction();
        }

        // Run planner
        ReconstructionPlannerUtility planner;
        ReconstructionPlannerInput input;
        input.reorganized_files = {reorg_file};
        // index_dir empty => midx is next to file

        auto plan = planner.process(input).get();

        // Verify plan
        REQUIRE(plan.files.size() == 1);
        CHECK(plan.files.count("/original/trace.pfw.gz") == 1);
        CHECK(plan.total_segments == 3);
        CHECK(plan.total_events == 400);

        const auto& recon = plan.files.at("/original/trace.pfw.gz");
        CHECK(recon.num_checkpoints == 3);
        CHECK(recon.event_hash == "abc123");
        REQUIRE(recon.checkpoint_segments.size() == 3);

        // Checkpoint 0
        REQUIRE(recon.checkpoint_segments.at(0).size() == 1);
        CHECK(recon.checkpoint_segments.at(0)[0].output_line_start == 0);
        CHECK(recon.checkpoint_segments.at(0)[0].output_line_end == 100);
        CHECK(recon.checkpoint_segments.at(0)[0].event_count == 100);

        // Checkpoint 1
        REQUIRE(recon.checkpoint_segments.at(1).size() == 1);
        CHECK(recon.checkpoint_segments.at(1)[0].output_line_start == 100);
        CHECK(recon.checkpoint_segments.at(1)[0].output_line_end == 250);
        CHECK(recon.checkpoint_segments.at(1)[0].event_count == 150);

        // Checkpoint 2
        REQUIRE(recon.checkpoint_segments.at(2).size() == 1);
        CHECK(recon.checkpoint_segments.at(2)[0].output_line_start == 250);
        CHECK(recon.checkpoint_segments.at(2)[0].output_line_end == 400);
        CHECK(recon.checkpoint_segments.at(2)[0].event_count == 150);

        fs::remove_all(test_dir);
    }

    TEST_CASE(
        "Multiple reorganized files merge "
        "correctly") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_recon_planner_merge")
                .string();
        fs::create_directories(test_dir);

        // Create two dummy reorganized files
        std::string io_file = test_dir + "/io.pfw.gz";
        std::string compute_file = test_dir + "/compute.pfw.gz";
        {
            std::ofstream(io_file) << "dummy";
            std::ofstream(compute_file) << "dummy";
        }

        // Create .midx for io.pfw.gz
        {
            std::string midx_path = determine_manifest_index_path(io_file, "");
            ManifestIndexDatabase midx(midx_path);
            midx.init_schema();
            midx.get_or_create_file_info(io_file, 0);

            midx.begin_transaction();
            queries::insert_provenance_info(midx.db(), "version", "1.0");
            queries::insert_provenance_info(midx.db(), "tool",
                                            "dftracer_organize");
            queries::insert_provenance_group(midx.db(), "io", "cat=POSIX");

            int fid = midx.get_file_info_id(io_file);
            queries::insert_provenance_source(
                midx.db(), fid, 0, "/original/trace.pfw.gz", 2, "hash1");

            // Segments for checkpoints 0 and 1
            queries::insert_provenance_segment(midx.db(), 0, 0, 0, 50, 50);
            queries::insert_provenance_segment(midx.db(), 0, 1, 50, 120, 70);

            midx.commit_transaction();
        }

        // Create .midx for compute.pfw.gz
        {
            std::string midx_path =
                determine_manifest_index_path(compute_file, "");
            ManifestIndexDatabase midx(midx_path);
            midx.init_schema();
            midx.get_or_create_file_info(compute_file, 0);

            midx.begin_transaction();
            queries::insert_provenance_info(midx.db(), "version", "1.0");
            queries::insert_provenance_info(midx.db(), "tool",
                                            "dftracer_organize");
            queries::insert_provenance_group(midx.db(), "compute", "cat=APP");

            int fid = midx.get_file_info_id(compute_file);
            queries::insert_provenance_source(
                midx.db(), fid, 0, "/original/trace.pfw.gz", 2, "hash1");

            // Segments for checkpoints 0 and 1
            queries::insert_provenance_segment(midx.db(), 0, 0, 0, 30, 30);
            queries::insert_provenance_segment(midx.db(), 0, 1, 30, 80, 50);

            midx.commit_transaction();
        }

        // Run planner with both files
        ReconstructionPlannerUtility planner;
        ReconstructionPlannerInput input;
        input.reorganized_files = {io_file, compute_file};

        auto plan = planner.process(input).get();

        // Should have 1 original file entry
        REQUIRE(plan.files.size() == 1);
        CHECK(plan.files.count("/original/trace.pfw.gz") == 1);

        const auto& recon = plan.files.at("/original/trace.pfw.gz");

        // Checkpoint 0: 2 segments (io + compute)
        REQUIRE(recon.checkpoint_segments.count(0) == 1);
        CHECK(recon.checkpoint_segments.at(0).size() == 2);

        // Checkpoint 1: 2 segments (io + compute)
        REQUIRE(recon.checkpoint_segments.count(1) == 1);
        CHECK(recon.checkpoint_segments.at(1).size() == 2);

        // Total segments: 4
        CHECK(plan.total_segments == 4);

        fs::remove_all(test_dir);
    }

    TEST_CASE("File without provenance is skipped") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_recon_planner_noprov")
                .string();
        fs::create_directories(test_dir);

        // Create a dummy file
        std::string reorg_file = test_dir + "/noprov.pfw.gz";
        {
            std::ofstream(reorg_file) << "dummy";
        }

        // Create .midx with NO provenance tables
        std::string midx_path = determine_manifest_index_path(reorg_file, "");
        {
            ManifestIndexDatabase midx(midx_path);
            midx.init_schema();
            midx.get_or_create_file_info(reorg_file, 0);
            // No provenance data inserted
        }

        // Run planner
        ReconstructionPlannerUtility planner;
        ReconstructionPlannerInput input;
        input.reorganized_files = {reorg_file};

        auto plan = planner.process(input).get();

        CHECK(plan.files.empty());
        CHECK(plan.total_segments == 0);
        CHECK(plan.total_events == 0);

        fs::remove_all(test_dir);
    }
}
