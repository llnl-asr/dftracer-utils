#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_dimension_stats.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <dftracer/utils/utilities/composites/dft/indexing/queries/queries.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <doctest/doctest.h>

#include <string>

#include "testing_utilities.h"

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::composites::dft::indexing;
using namespace dftracer::utils::utilities::composites::dft::indexing::queries;
using dftracer::utils::utilities::common::query::Query;
using dftracer::utils::utilities::indexer::IndexDatabase;
using dftracer::utils::utilities::indexer::internal::get_logical_path;

static void populate_test_idx(const std::string& idx_path,
                              const std::string& file_path) {
    IndexDatabase idx_db(idx_path);
    idx_db.init_base_schema();
    idx_db.init_bloom_schema();

    int fid =
        idx_db.get_or_create_file_info(get_logical_path(file_path), 12345);

    idx_db.begin_transaction();

    // Chunk 0: POSIX reads, dur 100-200
    {
        ChunkDimensionStats cat_ds;
        cat_ds.dimension = "cat";
        cat_ds.value_type = "string";
        cat_ds.observe("POSIX");
        cat_ds.observe("POSIX");
        insert_chunk_dimension_stats(idx_db.sql_db(), fid, 0, cat_ds);

        ChunkDimensionStats name_ds;
        name_ds.dimension = "name";
        name_ds.value_type = "string";
        name_ds.observe("read");
        name_ds.observe("read");
        insert_chunk_dimension_stats(idx_db.sql_db(), fid, 0, name_ds);

        ChunkDimensionStats dur_ds;
        dur_ds.dimension = "dur";
        dur_ds.value_type = "uint";
        dur_ds.observe("100");
        dur_ds.observe("200");
        insert_chunk_dimension_stats(idx_db.sql_db(), fid, 0, dur_ds);

        insert_index_dimension(idx_db.sql_db(), fid, "cat");
        insert_index_dimension(idx_db.sql_db(), fid, "name");
        insert_index_dimension(idx_db.sql_db(), fid, "dur");
    }

    // Chunk 1: STDIO writes, dur 500-600
    {
        ChunkDimensionStats cat_ds;
        cat_ds.dimension = "cat";
        cat_ds.value_type = "string";
        cat_ds.observe("STDIO");
        insert_chunk_dimension_stats(idx_db.sql_db(), fid, 1, cat_ds);

        ChunkDimensionStats name_ds;
        name_ds.dimension = "name";
        name_ds.value_type = "string";
        name_ds.observe("write");
        insert_chunk_dimension_stats(idx_db.sql_db(), fid, 1, name_ds);

        ChunkDimensionStats dur_ds;
        dur_ds.dimension = "dur";
        dur_ds.value_type = "uint";
        dur_ds.observe("500");
        dur_ds.observe("600");
        insert_chunk_dimension_stats(idx_db.sql_db(), fid, 1, dur_ds);
    }

    // Chunk 2: POSIX + MPI mixed, dur 50-1000
    {
        ChunkDimensionStats cat_ds;
        cat_ds.dimension = "cat";
        cat_ds.value_type = "string";
        cat_ds.observe("POSIX");
        cat_ds.observe("MPI");
        insert_chunk_dimension_stats(idx_db.sql_db(), fid, 2, cat_ds);

        ChunkDimensionStats name_ds;
        name_ds.dimension = "name";
        name_ds.value_type = "string";
        name_ds.observe("read");
        name_ds.observe("send");
        insert_chunk_dimension_stats(idx_db.sql_db(), fid, 2, name_ds);

        ChunkDimensionStats dur_ds;
        dur_ds.dimension = "dur";
        dur_ds.value_type = "uint";
        dur_ds.observe("50");
        dur_ds.observe("1000");
        insert_chunk_dimension_stats(idx_db.sql_db(), fid, 2, dur_ds);
    }

    idx_db.commit_transaction();
}

static ChunkPrunerOutput run_pruner(const std::string& idx_path,
                                    const std::string& file_path,
                                    const char* query_str) {
    auto q = Query::from_string(query_str);
    REQUIRE(q.has_value());

    ChunkPrunerInput input{idx_path, file_path, std::move(*q), nullptr};

    ChunkPrunerUtility pruner;
    return pruner.process(input).get();
}

TEST_SUITE("ChunkPrunerUtility") {
    TEST_CASE("Pruner - equality match via dictionary") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_pruner_eq").string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(idx_path, file_path);

        auto out = run_pruner(idx_path, file_path, R"(cat == "POSIX")");
        CHECK(out.success);
        CHECK(out.total_checkpoints == 3);
        // Chunks 0 and 2 have POSIX, chunk 1 has only STDIO
        CHECK(out.candidate_checkpoints.size() == 2);
        CHECK(out.candidate_checkpoints[0] == 0);
        CHECK(out.candidate_checkpoints[1] == 2);
    }

    TEST_CASE("Pruner - equality no match") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_pruner_eq_none")
                .string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(idx_path, file_path);

        auto out = run_pruner(idx_path, file_path, R"(cat == "HDF5")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.empty());
        CHECK_FALSE(out.file_may_match);
    }

    TEST_CASE("Pruner - in operator") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_pruner_in").string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(idx_path, file_path);

        auto out =
            run_pruner(idx_path, file_path, R"(cat in ["POSIX", "STDIO"])");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 3);
    }

    TEST_CASE("Pruner - not in operator") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_pruner_notin").string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(idx_path, file_path);

        // Chunk 0: only POSIX → excluded by not in ["POSIX"]
        // Chunk 1: only STDIO → kept
        // Chunk 2: POSIX + MPI → MPI not in list → kept
        auto out = run_pruner(idx_path, file_path, R"(cat not in ["POSIX"])");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 2);
        CHECK(out.candidate_checkpoints[0] == 1);
        CHECK(out.candidate_checkpoints[1] == 2);
    }

    TEST_CASE("Pruner - AND intersection") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_pruner_and").string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(idx_path, file_path);

        // cat == "POSIX" → chunks 0, 2
        // name == "read" → chunks 0, 2
        // AND → chunks 0, 2
        auto out = run_pruner(idx_path, file_path,
                              R"(cat == "POSIX" and name == "read")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 2);
    }

    TEST_CASE("Pruner - AND narrows results") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_pruner_and2").string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(idx_path, file_path);

        // cat == "POSIX" → chunks 0, 2
        // name == "send" → chunk 2 only
        // AND → chunk 2
        auto out = run_pruner(idx_path, file_path,
                              R"(cat == "POSIX" and name == "send")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 1);
        CHECK(out.candidate_checkpoints[0] == 2);
    }

    TEST_CASE("Pruner - OR union") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_pruner_or").string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(idx_path, file_path);

        // cat == "STDIO" → chunk 1
        // name == "send" → chunk 2
        // OR → chunks 1, 2
        auto out = run_pruner(idx_path, file_path,
                              R"(cat == "STDIO" or name == "send")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 2);
        CHECK(out.candidate_checkpoints[0] == 1);
        CHECK(out.candidate_checkpoints[1] == 2);
    }

    TEST_CASE("Pruner - NOT via dictionary") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_pruner_not").string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(idx_path, file_path);

        // cat == "STDIO" → chunk 1
        // NOT → chunks 0, 2
        auto out = run_pruner(idx_path, file_path, R"(not cat == "STDIO")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 2);
        CHECK(out.candidate_checkpoints[0] == 0);
        CHECK(out.candidate_checkpoints[1] == 2);
    }

    TEST_CASE("Pruner - range via min/max") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_pruner_range").string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(idx_path, file_path);

        // dur > "500": chunk 0 max=200 (skip), chunk 1 max=600 (keep),
        // chunk 2 max=1000 (keep)
        auto out = run_pruner(idx_path, file_path, R"(dur > "500")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 2);
        CHECK(out.candidate_checkpoints[0] == 1);
        CHECK(out.candidate_checkpoints[1] == 2);
    }

    TEST_CASE("Pruner - case-insensitive keywords") {
        std::string test_dir =
            dft_utils_test::make_unique_test_path("test_pruner_case").string();
        fs::create_directories(test_dir);
        std::string idx_path = test_dir + "/test.pfw.gz.idx";
        std::string file_path = "/fake/test.pfw.gz";
        populate_test_idx(idx_path, file_path);

        auto out = run_pruner(idx_path, file_path,
                              R"(cat == "POSIX" AND name == "send")");
        CHECK(out.success);
        CHECK(out.candidate_checkpoints.size() == 1);
    }
}
