#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/index/extensions/bloom_filter.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_planner_utility.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>

#include <string>

#include "testing_utilities.h"

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;
using namespace dftracer::utils::index::extensions;
using dftracer::utils::index::store::IndexDatabase;
using dftracer::utils::index::store::internal::get_logical_path;

// Helper: create a .idx with 4 checkpoints
// Checkpoint layout:
//   0: name={read,write}, cat={POSIX}
//   1: name={open,close}, cat={POSIX}
//   2: name={train}, cat={compute}
//   3: name={forward}, cat={compute,ai_framework}
static void populate_test_idx(const std::string& index_path,
                              const std::string& file_path) {
    IndexDatabase idx_db(index_path);
    auto writer = idx_db.begin_write();
    writer->init_schema();

    int fid = dftu_utils_test::register_test_file(
        *writer, get_logical_path(file_path), 40000);

    struct ChunkDims {
        std::vector<std::string> names;
        std::vector<std::string> cats;
    };

    ChunkDims chunks[] = {
        {{"read", "write"}, {"POSIX"}},
        {{"open", "close"}, {"POSIX"}},
        {{"train"}, {"compute"}},
        {{"forward"}, {"compute", "ai_framework"}},
    };

    BloomFilter file_name_bloom(100, 0.01);
    BloomFilter file_cat_bloom(100, 0.01);

    for (int ckpt = 0; ckpt < 4; ++ckpt) {
        BloomFilter name_bloom(100, 0.01);
        for (const auto& n : chunks[ckpt].names) {
            name_bloom.add(n);
            file_name_bloom.add(n);
        }
        auto name_blob = name_bloom.serialize();
        dftu_utils_test::put_chunk_bloom(
            *writer, fid, static_cast<std::uint64_t>(ckpt), "name", name_blob,
            name_bloom.num_entries());

        BloomFilter cat_bloom(100, 0.01);
        for (const auto& c : chunks[ckpt].cats) {
            cat_bloom.add(c);
            file_cat_bloom.add(c);
        }
        auto cat_blob = cat_bloom.serialize();
        dftu_utils_test::put_chunk_bloom(
            *writer, fid, static_cast<std::uint64_t>(ckpt), "cat", cat_blob,
            cat_bloom.num_entries());

        const auto m = static_cast<std::uint64_t>(ckpt);
        dftu_utils_test::index_records::put_gzip_member(
            *writer, fid,
            dftracer::utils::index::gzip::GzipMemberRecord{
                .member_idx = m,
                .c_offset = m * 5000,
                .c_size = 5000,
                .uc_offset = m * 10000,
                .uc_size = 10000,
                .first_line_num = m * 10 + 1,
                .last_line_num = m * 10 + 10,
            });
    }

    auto name_blob = file_name_bloom.serialize();
    dftu_utils_test::put_file_bloom(*writer, fid, "name", name_blob,
                                    file_name_bloom.num_entries());

    auto cat_blob = file_cat_bloom.serialize();
    dftu_utils_test::put_file_bloom(*writer, fid, "cat", cat_blob,
                                    file_cat_bloom.num_entries());

    dftu_utils_test::index_records::put_path(
        *writer, dftu_utils_test::IndexExtension::BLOOM, fid, "name");
    dftu_utils_test::index_records::put_path(
        *writer, dftu_utils_test::IndexExtension::BLOOM, fid, "cat");
    dftu_utils_test::mark_built(*writer, fid);
    writer->commit();
}

TEST_SUITE("ViewPlannerUtility") {
    TEST_CASE("ViewPlanner - IO view filters to POSIX chunks") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_view_builder_io")
                .string();
        fs::create_directories(test_dir);

        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        ViewPlannerInput input;
        input.with_view(ViewDefinition::io_view().with_include_metadata(false))
            .with_file_path(file_path)
            .with_index_path(index_path)
            .with_uncompressed_size(40000)
            .with_num_checkpoints(4);

        ViewPlannerUtility builder;
        auto output = builder(input).get();

        CHECK(output);
        CHECK(output->file_may_match);
        CHECK(output->total_checkpoints == 4);
        // IO view matches POSIX (checkpoints 0, 1) and names like read/write
        // (0, 1)
        CHECK(output->candidates.size() >= 2);
        CHECK(output->skipped_checkpoints >= 2);

        // Verify byte ranges are computed
        for (const auto& c : output->candidates) {
            CHECK(c.end_byte > c.start_byte);
        }

        fs::remove_all(test_dir);
    }

    TEST_CASE("ViewPlanner - Compute view filters to compute chunks") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_view_builder_compute")
                .string();
        fs::create_directories(test_dir);

        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        ViewPlannerInput input;
        input
            .with_view(
                ViewDefinition::compute_view().with_include_metadata(false))
            .with_file_path(file_path)
            .with_index_path(index_path)
            .with_uncompressed_size(40000)
            .with_num_checkpoints(4);

        ViewPlannerUtility builder;
        auto output = builder(input).get();

        CHECK(output);
        CHECK(output->file_may_match);
        // Compute view: cat={compute, comm, device, ai_framework, ai_root}
        // Matches checkpoints 2 and 3
        CHECK(output->candidates.size() >= 2);
        CHECK(output->skipped_checkpoints >= 2);

        fs::remove_all(test_dir);
    }

    TEST_CASE("ViewPlanner - File-level skip for absent values") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_view_builder_skip")
                .string();
        fs::create_directories(test_dir);

        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        ViewDefinition view;
        view.with_name("nonexistent")
            .with_query(R"(cat == "NONEXISTENT")")
            .with_include_metadata(false);

        ViewPlannerInput input;
        input.with_view(view)
            .with_file_path(file_path)
            .with_index_path(index_path)
            .with_uncompressed_size(40000)
            .with_num_checkpoints(4);

        ViewPlannerUtility builder;
        auto output = builder(input).get();

        CHECK(output);
        CHECK_FALSE(output->file_may_match);
        CHECK(output->candidates.empty());
        CHECK(output->skipped_checkpoints == 4);

        fs::remove_all(test_dir);
    }

    TEST_CASE("ViewPlanner - No bloom predicates returns all chunks") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_view_builder_nobl")
                .string();
        fs::create_directories(test_dir);

        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(index_path, file_path);

        ViewDefinition view;
        view.with_name("time_only").with_query(R"(ts >= 0 and ts <= 100000)");

        ViewPlannerInput input;
        input.with_view(view)
            .with_file_path(file_path)
            .with_index_path(index_path)
            .with_uncompressed_size(40000)
            .with_num_checkpoints(4);

        ViewPlannerUtility builder;
        auto output = builder(input).get();

        CHECK(output);
        CHECK(output->file_may_match);
        CHECK(output->candidates.size() == 4);
        CHECK(output->skipped_checkpoints == 0);

        fs::remove_all(test_dir);
    }

    TEST_CASE("ViewPlanner - No index reads the file as one unit") {
        ViewDefinition view;
        view.with_name("no_bidx");

        ViewPlannerInput input;
        input.with_view(view)
            .with_file_path("/fake/file.pfw.gz")
            .with_index_path("")  // No bloom index
            .with_uncompressed_size(30000)
            .with_num_checkpoints(3);

        ViewPlannerUtility builder;
        auto output = builder(input).get();

        CHECK(output);
        CHECK(output->file_may_match);
        REQUIRE(output->candidates.size() == 1);
        CHECK(output->candidates[0].start_byte == 0);
        CHECK(output->candidates[0].end_byte == 30000);
        CHECK(output->skipped_checkpoints == 0);
    }

    TEST_CASE("ViewPlanner - Zero checkpoints defaults to 1") {
        ViewDefinition view;
        view.with_name("zero_ckpt");

        ViewPlannerInput input;
        input.with_view(view)
            .with_file_path("/fake/file.pfw.gz")
            .with_index_path("")
            .with_uncompressed_size(10000)
            .with_num_checkpoints(0);

        ViewPlannerUtility builder;
        auto output = builder(input).get();

        CHECK(output);
        CHECK(output->total_checkpoints == 1);
        REQUIRE(output->candidates.size() == 1);
        CHECK(output->candidates[0].start_byte == 0);
        CHECK(output->candidates[0].end_byte == 10000);
    }

    TEST_CASE("ViewPlanner - Dimension alias resolution") {
        std::string test_dir =
            dftu_utils_test::make_unique_test_path("test_view_builder_alias")
                .string();
        fs::create_directories(test_dir);

        std::string index_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";

        IndexDatabase idx_db(index_path);
        {
            auto writer = idx_db.begin_write();
            writer->init_schema();
            int fid = dftu_utils_test::register_test_file(
                *writer, get_logical_path(file_path), 10000);

            BloomFilter fhash_bloom(100, 0.01);
            fhash_bloom.add("hash123");
            auto blob = fhash_bloom.serialize();

            dftu_utils_test::put_file_bloom(*writer, fid, "fhash", blob,
                                            fhash_bloom.num_entries());
            dftu_utils_test::put_chunk_bloom(*writer, fid, 0, "fhash", blob,
                                             fhash_bloom.num_entries());
            dftu_utils_test::index_records::put_path(
                *writer, dftu_utils_test::IndexExtension::BLOOM, fid, "fhash");
            dftu_utils_test::index_records::put_dict_row(
                *writer, "file", "hash123", {{"path", "/data/file.h5"}});
            dftu_utils_test::mark_built(*writer, fid);
            writer->commit();
        }

        // Use "file" alias which should resolve to "fhash"
        ViewDefinition view;
        view.with_name("alias_test");

        ViewPlannerInput input;
        input.with_view(view)
            .with_file_path(file_path)
            .with_index_path(index_path)
            .with_uncompressed_size(10000)
            .with_num_checkpoints(1);

        ViewPlannerUtility builder;
        auto output = builder(input).get();

        CHECK(output);
        CHECK(output->file_may_match);
        CHECK(output->candidates.size() == 1);

        fs::remove_all(test_dir);
    }
}
